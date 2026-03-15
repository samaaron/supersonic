/*
 * FuzzyMatch.h — Multi-token case-insensitive substring matcher
 */
#pragma once

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

// Splits `pattern` into whitespace-delimited tokens and returns the best
// candidate where ALL tokens appear as case-insensitive substrings.
// Prefers the shortest matching candidate (most specific).
inline std::string fuzzyMatch(const std::string& pattern,
                               const std::vector<std::string>& candidates) {
    if (pattern.empty()) return "";

    // Tokenise the pattern
    std::vector<std::string> tokens;
    {
        std::string lower = pattern;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        std::istringstream iss(lower);
        std::string tok;
        while (iss >> tok)
            tokens.push_back(tok);
    }
    if (tokens.empty()) return "";

    std::string bestMatch;

    for (auto& candidate : candidates) {
        std::string lowerCandidate = candidate;
        std::transform(lowerCandidate.begin(), lowerCandidate.end(),
                       lowerCandidate.begin(), ::tolower);

        // All tokens must appear as substrings
        bool allFound = true;
        for (auto& tok : tokens) {
            if (lowerCandidate.find(tok) == std::string::npos) {
                allFound = false;
                break;
            }
        }

        if (allFound) {
            if (bestMatch.empty() || candidate.size() < bestMatch.size())
                bestMatch = candidate;
        }
    }
    return bestMatch;
}
