#pragma once
#include <cstdint>
#include <vector>
#include "BikeData.h"

namespace mxb {
// Owns a temporary parameter view and every mutable curve it references.
// Used only while building the local client's verification reply.
struct StockBikeView {
    std::vector<uint8_t> bytes;
    std::vector<std::vector<CurvePoint>> curves;
    void* data() { return bytes.data(); }
};

// Returns false if this bike has no captured stock data or its layout changed.
// Never writes to the live bike or its external curve arrays.
bool BuildStockBikeView(void* liveBike, StockBikeView& result, const char** failureReason = nullptr);
}
