// Copyright (c) 2019-2020 akshaynexus
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include "blacklist.h"
#include "util.h"
std::vector<std::string> blacklistedAddrs {
    "iE7gYFQbMXT418C5epsF9hKFi9zhgd6yKU",
    "iBSRobL3D6K4vfGgn9jojGojfuo6yAiHnp",
    "iHo1rsD3GTR72XLidY16HVh7xUTAxVDVgX"
};
bool ContainsBlacklistedAddr(std::string addr){
    //Iterate through blacklisted addresses
    for (Iter it = blacklistedAddrs.begin(); it!=blacklistedAddrs.end(); ++it) {
        if(*it == addr) {
            LogPrintf("ContainsBlacklistedAddr() Found Blacklisted addr %s\n", addr);
            return true;
        }
    }
    return false;
}