#pragma once

namespace meld {

//! States of compared and version-controlled files: ignored, new, normal,
//! ignored changes, error, placeholder, vc added, vc modified, vc renamed,
//! vc conflict, vc removed, locally removed, end
enum TreeState {
    StateIgnored,
    StateNone,
    StateNormal,
    StateNoChange,
    StateError,
    StateEmpty,
    StateNew,
    StateModified,
    StateRenamed,
    StateConflict,
    StateRemoved,
    StateMissing,
    StateNonExist,
    StateSpinner,
    StateMax,
};

} // namespace meld
