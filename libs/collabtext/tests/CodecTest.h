#pragma once
#include <collabtext/ActorId.h>
#include <cstdio>
#include <cstdlib>
// Test/consumer assertion, active in Release too. Never erases a failed codec.
template<class T> T checked_codec(CollabText::Result<T> result) {
    if (!result.ok()) {
        std::fprintf(stderr, "codec check failed: %.*s: %.*s\n",
            int(CollabText::to_string(result.error().code()).size()), CollabText::to_string(result.error().code()).data(),
            int(result.error().message().size()), result.error().message().data());
        std::abort();
    }
    return std::move(result).value();
}
