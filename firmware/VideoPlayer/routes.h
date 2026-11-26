#pragma once

static const char* const VideoRoutes[8][8] = {
    {"", "W", "WW", "WWHW", "WWWW", "WHHWWWW", "WHHWWWWWWW", "WWWWWWWWWWWWW"},
    {"BBBB", "", "W", "WWWHHB", "WWW", "WWWW", "WWWWWWW", "WHHWWWWWWWWWWWW"},
    {"BBBBBBBB", "BBB", "", "WBWW", "WWWHB", "BWWWW", "WWWWWW", "WWWWWWWWWWWW"},
    {"BBBBBBBB", "BBBB", "BB", "", "WBWW", "WWHW", "WWWWWW", "WWWWWWWWWWWW"},
    {"BBBBBBBB", "BBBBB", "BBB", "B", "", "BWWW", "WWWWW", "WWWWWHHWWWWWW"},
    {"BBBBBBBBBBBBBBB", "BBBHHBBB", "BBB", "BB", "B", "", "WWWW", "WWWWWWWWWW"},
    {"BBBBBBBBBBBBBBB", "BBBBBBBB", "BBBBB", "BBHHBB", "BBHHB", "BB", "", "WWWWWW"},
    {"BBBBBBBBBBBBBBB", "BBBBBBBBB", "BBBBBB", "BBBBB", "BBHHBBHHB", "BBHHBB", "BB", ""},
};
