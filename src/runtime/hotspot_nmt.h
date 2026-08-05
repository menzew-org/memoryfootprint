/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Platform-neutral parser for HotSpot Native Memory Tracking summaries.
 */
#pragma once

#include <string>
#include <vector>

namespace memftprt::hotspot_nmt {

struct Category {
    std::string name;
    unsigned long reserved_kb = 0;
    unsigned long committed_kb = 0;
};

// Unknown lines are ignored so new HotSpot prose does not invalidate known
// category rows. An empty result means the response was not a usable summary.
std::vector<Category> parse_summary(const std::string& raw_output);

}  // namespace memftprt::hotspot_nmt
