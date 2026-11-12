#pragma once

namespace meld::vc {

class Plugin;

const Plugin &gitPlugin();        // git.cpp
const Plugin &mercurialPlugin();  // mercurial.cpp
const Plugin &bazaarPlugin();     // bzr.cpp
const Plugin &subversionPlugin(); // svn.cpp
const Plugin &darcsPlugin();      // darcs.cpp
const Plugin &cvsPlugin();        // cvs.cpp

} // namespace meld::vc
