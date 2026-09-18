// Shared types for the package - the "typed" half of "typed ES module
// package": everything a consumer touches has a real interface instead of
// the `any` an untyped Embind wrapper would otherwise force on callers.
/** ac3::DownmixTarget's own numeric order (output.hpp) - kept in sync by hand, there being only four values. */
export var DownmixTarget;
(function (DownmixTarget) {
    DownmixTarget[DownmixTarget["AsCoded"] = 0] = "AsCoded";
    DownmixTarget[DownmixTarget["LoRo"] = 1] = "LoRo";
    DownmixTarget[DownmixTarget["LtRt"] = 2] = "LtRt";
    DownmixTarget[DownmixTarget["Mono"] = 3] = "Mono";
})(DownmixTarget || (DownmixTarget = {}));
//# sourceMappingURL=types.js.map