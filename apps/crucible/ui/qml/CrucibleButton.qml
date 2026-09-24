import Ac3ForgeCrucible

// The family's flat bordered button, which this file used to carry its own
// copy of. It now lives in apps/gui/qml/AppButton.qml and is staged into
// every family member's module by cmake/SharedFamilyQml.cmake, the same way
// Theme/Card/SegmentedControl already were - two copies of one control is
// how the two come to disagree (that file's own header says so).
//
// Kept as a name rather than folded away entirely so Crucible's own QML and
// its tests, which call this control CrucibleButton throughout, do not all
// have to change in the same commit that shares it.
AppButton { }
