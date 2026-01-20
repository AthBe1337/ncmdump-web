#include <vector>
#include <string>
#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <emscripten/emscripten.h>
#include "ncmcrypt.h"

using namespace emscripten;

std::string g_lastError = "";

std::string getLastError() {
    return g_lastError;
}

val decryptNCM(uintptr_t inputDataPtr, size_t inputDataSize) {
    g_lastError = "";
    
    try {
        const uint8_t* input_data_raw = reinterpret_cast<const uint8_t*>(inputDataPtr);

        NeteaseCrypt crypt(input_data_raw, inputDataSize);
        crypt.DumpToMemory();
        crypt.FixMetadata();

        const std::vector<uint8_t>& finalDecryptedData = crypt.getDecryptedAudioData();

        return val(typed_memory_view(finalDecryptedData.size(), finalDecryptedData.data()));

    } catch (const std::exception& e) {
        g_lastError = e.what();
        return val::null();
    } catch (...) {
        g_lastError = "Unknown error occurred inside WASM";
        return val::null();
    }
}

EMSCRIPTEN_BINDINGS(ncmdump_module) {
    function("decryptNCM", &decryptNCM);
    function("getLastError", &getLastError);
}