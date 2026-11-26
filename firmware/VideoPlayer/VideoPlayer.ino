#include <Arduino.h>
SET_LOOP_TASK_STACK_SIZE(32768);

#include <esp_heap_caps.h>
#include <miniz.h>
#include "Ssd1680.h"
#include "routes.h"

constexpr unsigned Gates=panel::Width, Sources=panel::RamSources, MaskBytes=panel::PlaneBytes;
constexpr unsigned Pixels=Gates*Sources, FrameBytes=Pixels/2;
constexpr unsigned Shades=8, MaxRoute=18, FramePeriod=40000;
constexpr unsigned Chunk=4096, MaxAsset=6*1024*1024;
constexpr uint8_t Reference[Shades]={0,1,2,3,4,5,8,17};
constexpr unsigned DoseUnit=4, MaxDose=17*DoseUnit;
constexpr uint16_t Estimated=0x8000;
constexpr uint8_t Unknown=255;

// An in-progress exact recipe has a known target and a record of the dose
// already delivered before this step. An interrupted recipe loses certainty.
struct Step {uint16_t next; uint8_t mask, target, dose, source;};
struct Estimate {uint8_t next, mask;};
static Step steps[Shades+Shades*Shades*MaxRoute];
static Estimate estimates[Shades][MaxDose+1];
static uint16_t startRoute[Shades][Shades];
static uint16_t pixel[Pixels];
static uint8_t frame[FrameBytes], decoded[FrameBytes], goal[Pixels];
static uint8_t* video=nullptr;
static unsigned videoLength=0, videoFrames=0;
static uint32_t expectedFramesCrc=0;
static bool canceled=false;

static uint16_t le16(const uint8_t* p){return p[0]|uint16_t(p[1])<<8;}
static uint32_t le32(const uint8_t* p){return le16(p)|uint32_t(le16(p+2))<<16;}

static void pollStop(){if(Serial.peek()=='!'){Serial.read();canceled=true;}}

static bool initRoutes(){
 unsigned used=Shades;
 for(unsigned a=0;a<Shades;++a){
  steps[a]={uint16_t(a),0,uint8_t(a),uint8_t(Reference[a]*DoseUnit),uint8_t(a)};
  for(unsigned dose=0;dose<=MaxDose;++dose){
   const int remaining=int(Reference[a])*int(DoseUnit)-int(dose);
   const int width=remaining>0?DoseUnit:dose>=Reference[6]*DoseUnit?10:5;
   if(abs(remaining)*2<width)estimates[a][dose]={uint8_t(dose),0};
   else estimates[a][dose]={uint8_t(constrain(int(dose)+(remaining>0?width:-width),0,int(MaxDose))),
                           uint8_t(remaining>0?1:2)};
  }
  for(unsigned b=0;b<Shades;++b){
   if(a==b){startRoute[a][b]=a;continue;}
   const char* recipe=VideoRoutes[a][b];
   const unsigned length=strlen(recipe);
   if(!length||length>MaxRoute||used+length>sizeof steps/sizeof *steps)return false;
   startRoute[a][b]=used;
   int net=0;
   for(unsigned i=0;i<length;++i){
    const char pulse=recipe[i];
    if(pulse=='W')++net;
    else if(pulse=='B')--net;
    else if(pulse!='H')return false;
   }
   int delivered=Reference[a]*256;
   const int width=max(1,abs((int(Reference[b])-Reference[a])*256)/max(1,abs(net)));
   uint8_t source=a;
   for(unsigned i=0;i<length;++i){
    const char pulse=recipe[i];
    const uint8_t mask=pulse=='W'?1:pulse=='B'?2:0;
    steps[used]={uint16_t(i+1==length?b:used+1),mask,uint8_t(b),uint8_t((delivered+32)/64),source};
    if(mask){
     delivered=constrain(delivered+(mask==1?width:-width),0,17*256);
     source=Unknown;
    }
    ++used;
   }
  }
 }
 return true;
}

static bool receiveAsset(unsigned size,uint32_t crcExpected){
 if(size<18||size>MaxAsset)return false;
 free(video);
 video=nullptr;
 videoLength=videoFrames=0;
 video=(uint8_t*)heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 if(!video)return false;
 Serial.printf("PV READY chunk=%u\n",Chunk);
 unsigned at=0;
 while(at<size){
  const unsigned end=min(size,at+Chunk);
  uint32_t last=millis();
  while(at<end){
   const size_t n=Serial.read(video+at,end-at);
   if(n){at+=n;last=millis();}
   else if(millis()-last>3000){free(video);video=nullptr;return false;}
   else delay(1);
  }
  Serial.printf("PV ACK bytes=%u\n",at);
 }
 const uint32_t crc=mz_crc32(0,video,size);
 if(crc!=crcExpected||memcmp(video,"FRV7",4)||le32(video+8)!=25){
  Serial.printf("PV REJECT crc=%08lx\n",(unsigned long)crc);
  free(video);video=nullptr;return false;
 }
 videoLength=size;
 videoFrames=le32(video+4);
 expectedFramesCrc=le32(video+12);
 Serial.printf("PV LOADED bytes=%u frames=%u crc=%08lx\n",size,videoFrames,(unsigned long)crc);
 return true;
}

static bool decodeFrame(unsigned& cursor,uint32_t& crc){
 if(cursor+2>videoLength)return false;
 const uint16_t code=le16(video+cursor);
 cursor+=2;
 const unsigned size=code&0x7fff;
 if(!size||size>videoLength-cursor)return false;
 const bool delta=(code&0x8000)!=0;
 auto* destination=delta?decoded:frame;
 if(tinfl_decompress_mem_to_mem(destination,FrameBytes,video+cursor,size,0)!=FrameBytes)return false;
 if(delta){for(unsigned i=0;i<FrameBytes;++i)frame[i]^=decoded[i];}
 cursor+=size;
 crc=mz_crc32(crc,frame,FrameBytes);
 for(unsigned i=0;i<FrameBytes;++i){
  goal[2*i]=frame[i]&7;
  goal[2*i+1]=(frame[i]>>4)&7;
 }
 return true;
}

static void prepareMasks(unsigned pass){
 auto* dark=panel::darkMask();
 auto* light=panel::lightMask();
 for(unsigned byte=0;byte<MaskBytes;++byte){
  unsigned bits=0;
  for(unsigned b=0;b<8;++b){
   const unsigned dose=Reference[goal[byte*8+b]];
   bits=(bits<<1)|(pass>=17-dose);
  }
  dark[byte]=bits;
  light[byte]=0;
 }
}

static bool nextMasks(unsigned& abandoned){
 auto* dark=panel::darkMask();
 auto* light=panel::lightMask();
 bool active=false;
 for(unsigned byte=0;byte<MaskBytes;++byte){
  unsigned d=0,w=0;
  for(unsigned b=0;b<8;++b){
   const unsigned index=byte*8+b;
   const uint8_t target=goal[index];
   uint16_t state=pixel[index];
   if(state<Shades)state=startRoute[state][target];
   else if(!(state&Estimated)&&steps[state].target!=target){
    const Step old=steps[state];
    state=old.source==Unknown?uint16_t(Estimated|old.dose):startRoute[old.source][target];
    ++abandoned;
   }
   uint8_t mask;
   if(state&Estimated){
    const Estimate step=estimates[target][state&~Estimated];
    active|=step.mask!=0;
    mask=step.mask;
    pixel[index]=Estimated|step.next;
   }else{
    const Step step=steps[state];
    active|=state>=Shades;
    mask=step.mask;
    pixel[index]=step.next;
   }
   d=(d<<1)|(mask&1);
   w=(w<<1)|(mask>>1);
  }
  dark[byte]=d;
  light[byte]=w;
 }
 return active;
}

static bool modelAtTarget(unsigned index){
 const uint16_t state=pixel[index];
 const uint8_t target=goal[index];
 if(state&Estimated)return !estimates[target][state&~Estimated].mask;
 return state==target;
}

static void waitUntil(uint32_t target){
 while(!canceled&&int32_t(target-micros())>0){
  pollStop();
  const int32_t left=target-micros();
  if(left>1300)delay(1);
  else if(left>0)delayMicroseconds(min(left,int32_t(150)));
 }
}

static void playback(){
 if(!video||!videoFrames||videoFrames>15000||!initRoutes()){Serial.println("PV ERROR no_asset_or_routes");return;}
 canceled=false;
 unsigned cursor=16,scans=0,visibleScans=0,submitted=0,late=0,maxScan=0;
 unsigned exactFrames=0,peakLate=0,matches=0,samples=0;
 unsigned abandoned=0;
 uint32_t crc=0;
 memset(frame,0,sizeof frame);
 if(!decodeFrame(cursor,crc)){Serial.println("PV ERROR decode_first");return;}
 if(!panel::prepare(pollStop)){Serial.println("PV ERROR panel_begin");return;}
 bool ok=true;
 for(unsigned pass=0;pass<17&&ok&&!canceled;++pass){
  prepareMasks(pass);
  ok=panel::scan();
  scans+=ok;
  pollStop();
 }
 for(unsigned i=0;i<Pixels;++i)pixel[i]=goal[i];
 Serial.printf("PV START fr=7 fps=25 frames=%u latest=1 routes=56\n",videoFrames);
 const uint32_t started=micros();
 for(unsigned index=0;index<videoFrames&&ok&&!canceled;++index){
  const uint32_t tick=started+index*FramePeriod;
  waitUntil(tick);
  if(canceled)break;
  const unsigned delayUs=micros()-tick;
  peakLate=max(peakLate,delayUs);
  late+=delayUs>1000;
  if(index&&!decodeFrame(cursor,crc)){ok=false;break;}
  ++submitted;
  const uint32_t deadline=tick+FramePeriod;
  while(!canceled&&int32_t(deadline-micros())>int32_t(max(maxScan,12500u)+350)){
   const uint32_t scanStart=micros();
   const bool active=nextMasks(abandoned);
   if(!active)break;
   if(!(ok=panel::scan()))break;
   maxScan=max(maxScan,unsigned(micros()-scanStart));
   ++scans;
   ++visibleScans;
   pollStop();
  }
  if(index%25==0||index+1==videoFrames){
   unsigned ready=0;
   for(unsigned i=0;i<Pixels;++i)ready+=modelAtTarget(i);
   matches+=ready;
   ++samples;
   exactFrames+=ready==Pixels;
   Serial.printf("PV PROGRESS frame=%u/%u scans=%u ready=%u late=%u\n",submitted,videoFrames,visibleScans,ready,late);
  }
 }
 if(ok&&!canceled)waitUntil(started+videoFrames*FramePeriod);
 const uint32_t duration=micros()-started;
 // Keep the final target active briefly so the last visible scene can settle.
 for(unsigned tail=0;ok&&!canceled&&tail<35;++tail){
  if(!nextMasks(abandoned))break;
  ok=panel::scan();
  scans+=ok;
  pollStop();
 }
 if(ok){panel::clearMasks();ok=panel::scan();scans+=ok;}
 const bool slept=panel::finish(ok);
 const bool sourceOk=cursor==videoLength&&crc==expectedFramesCrc;
 Serial.printf("PV %s submitted=%u frames=%u duration_us=%lu scans=%u max_step_us=%u late=%u peak_late_us=%u sampled_frames=%u exact_samples=%u matching_pixels=%u crc=%08lx expected=%08lx retargeted=%u\n",
  canceled?"STOP":ok&&slept&&sourceOk?"END":"ERROR",submitted,videoFrames,(unsigned long)duration,
  visibleScans,maxScan,late,peakLate,samples,exactFrames,matches,(unsigned long)crc,(unsigned long)expectedFramesCrc,abandoned);
}

void setup(){
 Serial.setRxBufferSize(8192);
 Serial.begin(460800);
}

void loop(){
 static char line[64];
 static unsigned cursor=0;
 while(Serial.available()){
  const char c=Serial.read();
  if(c=='!'){cursor=0;continue;}
  if(c=='\r')continue;
  if(c!='\n'){if(cursor+1<sizeof line)line[cursor++]=c;continue;}
  line[cursor]=0;
  cursor=0;
  unsigned size;
  unsigned long crc;
  if(!strcmp(line,"HELLO"))Serial.println("PV FR7-VIDEO READY");
  else if(!strcmp(line,"PLAY"))playback();
  else if(sscanf(line,"LOAD %u %lx",&size,&crc)==2){if(!receiveAsset(size,crc))Serial.println("PV ERROR upload");}
  else if(*line)Serial.println("PV ERROR command");
 }
 delay(1);
}
