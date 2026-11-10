// ViewZoom.cpp — see ViewZoom.h.
#include "ViewZoom.h"

#include "Json.h"

#include <algorithm>
#include <cmath>

namespace ViewZoom {

double size(double base, double zoom) {
    return std::max(kMinSize, std::min(kMaxSize, base + zoom));
}

bool step(double base, double zoom, double points, double* out) {
    double next = points == 0 ? 0 : zoom + points;
    // The zoom keeps the whole size within the limits.
    next = std::max(kMinSize - base, std::min(kMaxSize - base, next));
    if (next == zoom && points != 0) return false;
    if (out) *out = next;
    return true;
}

double previewScale(double zoom) { return std::max(0.5, (kPreviewBase + zoom) / kPreviewBase); }

double zoomed(double points, double zoom) {
    return std::round(points * previewScale(zoom) * 2) / 2;
}

double pageStep(double zoom, int dir) {
    // Safari's View > Zoom In and Zoom Out.
    static const double kSteps[] = {0.5, 0.67, 0.75, 0.85, 1.0, 1.15,
                                    1.25, 1.5, 1.75, 2.0, 2.5, 3.0};
    const size_t n = sizeof kSteps / sizeof kSteps[0];
    if (dir > 0) {
        for (size_t i = 0; i < n; i++)
            if (kSteps[i] > zoom + 0.004) return kSteps[i];
        return std::max(zoom, kSteps[n - 1]);
    }
    if (dir < 0) {
        for (size_t i = n; i-- > 0;)
            if (kSteps[i] < zoom - 0.004) return kSteps[i];
        return std::min(zoom, kSteps[0]);
    }
    return 1.0;
}

std::string encode(const State& s) {
    return Json::object({{"editor", s.editor}, {"terminal", s.terminal}}).dump();
}

State decode(const std::string& json) {
    State s;
    Json root;
    if (!Json::parse(json, root) || !root.isObject()) return s;
    // No base is under the smallest size or over the largest, so no zoom
    // that can be used is past their difference.
    const double most = kMaxSize - kMinSize;
    auto read = [most](const Json& v) {
        const double z = v.asNumber(0);
        return std::isfinite(z) ? std::max(-most, std::min(most, std::round(z))) : 0.0;
    };
    s.editor = read(root["editor"]);
    s.terminal = read(root["terminal"]);
    return s;
}

bool Wrap::on(bool setting) {
    const int now = setting ? 1 : 0;
    // The setting changed since the toggle: the setting's new value wins.
    if (override_ >= 0 && now != setting_) override_ = -1;
    return override_ >= 0 ? override_ == 1 : setting;
}

void Wrap::toggle(bool setting) {
    const bool wraps = on(setting);
    setting_ = setting ? 1 : 0;
    override_ = wraps ? 0 : 1;
}

}  // namespace ViewZoom
