#include "ncmcrypt.h"
#include "aes.h"
#include "base64.h"
#include "cJSON.h"
#include "color.h"

#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include <iostream>
#include <cstring>

#include "taglib/toolkit/tbytevectorstream.h"
#include "taglib/mpeg/mpegfile.h"
#include "taglib/flac/flacfile.h"
#include "taglib/mpeg/id3v2/frames/attachedpictureframe.h"
#include "taglib/mpeg/id3v2/id3v2tag.h"
#include "taglib/tag.h"

#pragma warning(disable:4267)
#pragma warning(disable:4244)

const unsigned char NeteaseCrypt::sCoreKey[17] = {0x68, 0x7A, 0x48, 0x52, 0x41, 0x6D, 0x73, 0x6F, 0x35, 0x6B, 0x49, 0x6E, 0x62, 0x61, 0x78, 0x57, 0};
const unsigned char NeteaseCrypt::sModifyKey[17] = {0x23, 0x31, 0x34, 0x6C, 0x6A, 0x6B, 0x5F, 0x21, 0x5C, 0x5D, 0x26, 0x30, 0x55, 0x3C, 0x27, 0x28, 0};
const unsigned char NeteaseCrypt::mPng[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

static void aesEcbDecrypt(const unsigned char *key, std::string &src, std::string &dst) {
    int n, i;
    unsigned char out[16];
    n = src.length() >> 4;
    dst.clear();
    AES aes(key);
    for (i = 0; i < n - 1; i++) {
        aes.decrypt((unsigned char *)src.c_str() + (i << 4), out);
        dst += std::string((char *)out, 16);
    }
    aes.decrypt((unsigned char *)src.c_str() + (i << 4), out);
    char pad = out[15];
    if (pad > 16) pad = 0;
    dst += std::string((char *)out, 16 - pad);
}

static void replace(std::string &str, const std::string &from, const std::string &to) {
    if (from.empty()) return;
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

NeteaseMusicMetadata::~NeteaseMusicMetadata() { cJSON_Delete(mRaw); }

NeteaseMusicMetadata::NeteaseMusicMetadata(const std::string& json_str) : mRaw(nullptr) {
    mRaw = cJSON_Parse(json_str.c_str());
    if (!mRaw) {
        std::cerr << BOLDRED << "[Error] " << RESET << "Failed to parse JSON metadata: " << json_str << std::endl;
        return;
    }

    cJSON *swap;
    int artistLen;

    swap = cJSON_GetObjectItem(mRaw, "musicName");
    if (swap && cJSON_IsString(swap)) mName = std::string(cJSON_GetStringValue(swap));

    swap = cJSON_GetObjectItem(mRaw, "album");
    if (swap && cJSON_IsString(swap)) mAlbum = std::string(cJSON_GetStringValue(swap));

    swap = cJSON_GetObjectItem(mRaw, "artist");
    if (swap && cJSON_IsArray(swap)) {
        artistLen = cJSON_GetArraySize(swap);
        for (int i = 0; i < artistLen; i++) {
            auto artist_array_item = cJSON_GetArrayItem(swap, i);
            if (artist_array_item && cJSON_IsArray(artist_array_item) && cJSON_GetArraySize(artist_array_item) > 0) {
                auto artist_name_item = cJSON_GetArrayItem(artist_array_item, 0);
                if (artist_name_item && cJSON_IsString(artist_name_item)) {
                    if (!mArtist.empty()) mArtist += "/";
                    mArtist += std::string(cJSON_GetStringValue(artist_name_item));
                }
            }
        }
    }

    swap = cJSON_GetObjectItem(mRaw, "bitrate");
    if (swap && cJSON_IsNumber(swap)) mBitrate = swap->valueint;

    swap = cJSON_GetObjectItem(mRaw, "duration");
    if (swap && cJSON_IsNumber(swap)) mDuration = swap->valueint;

    swap = cJSON_GetObjectItem(mRaw, "format");
    if (swap && cJSON_IsString(swap)) mFormat = std::string(cJSON_GetStringValue(swap));
}



int NeteaseCrypt::readFromBuffer(char *s, std::streamsize n) {
    if (mReadPos + n > mInputBuffer.size()) {
        throw std::runtime_error("Attempt to read beyond buffer end or EOF reached.");
    }
    std::copy(mInputBuffer.begin() + mReadPos, mInputBuffer.begin() + mReadPos + n, s);
    mReadPos += n;
    return n;
}

bool NeteaseCrypt::seekInBuffer(std::streamsize offset, std::ios_base::seekdir way) {
    size_t newPos;
    if (way == std::ios_base::cur) {
        newPos = mReadPos + offset;
    } else if (way == std::ios_base::beg) {
        newPos = offset;
    } else if (way == std::ios_base::end) {
        newPos = mInputBuffer.size() + offset;
    } else {
        return false;
    }

    if (newPos > mInputBuffer.size() || (offset < 0 && newPos > mReadPos)) { // Added check for negative offset leading to overflow
        return false;
    }
    mReadPos = newPos;
    return true;
}

bool NeteaseCrypt::isNcmFileInMemory() {
    if (mInputBuffer.size() < 8) return false;

    unsigned int header1, header2;

    header1 = *reinterpret_cast<const unsigned int*>(mInputBuffer.data());
    header2 = *reinterpret_cast<const unsigned int*>(mInputBuffer.data() + 4);


    const char expected_magic[8] = {0x43, 0x54, 0x45, 0x4E, 0x46, 0x44, 0x41, 0x4D}; // "CTENFDAM"

    if (memcmp(mInputBuffer.data(), expected_magic, 8) != 0) {
        return false;
    }

    return true;
}

void NeteaseCrypt::buildKeyBox(const unsigned char *key, int keyLen)
{
    int i;
    for (i = 0; i < 256; ++i) {
        mKeyBox[i] = (unsigned char)i;
    }

    unsigned char swap = 0;
    unsigned char c = 0;
    unsigned char last_byte = 0;
    unsigned char key_offset = 0;

    for (i = 0; i < 256; ++i) {
        swap = mKeyBox[i];
        c = ((swap + last_byte + key[key_offset++]) & 0xff);
        if (key_offset >= keyLen)
            key_offset = 0;
        mKeyBox[i] = mKeyBox[c];
        mKeyBox[c] = swap;
        last_byte = c;
    }
}

std::string NeteaseCrypt::mimeType(std::string &data) {
    if (memcmp(data.c_str(), mPng, 8) == 0) return "image/png";
    return "image/jpeg";
}

void NeteaseCrypt::FixMetadata() {
    if (mDecryptedAudioData.empty()) {
        std::cout << BOLDYELLOW << "[Warn] " << RESET << "No decrypted audio data available for metadata fixing." << std::endl;
        return;
    }

    TagLib::ByteVector audioByteVector(
        reinterpret_cast<const char*>(mDecryptedAudioData.data()),
        mDecryptedAudioData.size()
    );
    TagLib::ByteVectorStream audioStream(audioByteVector);
    TagLib::File *audioFile = nullptr;
    TagLib::Tag *tag = nullptr;
    TagLib::ByteVector imageByteVector(mImageData.c_str(), mImageData.length());

    if (mFormat == NeteaseCrypt::MP3) {
        audioFile = new TagLib::MPEG::File(&audioStream);
        tag = dynamic_cast<TagLib::MPEG::File *>(audioFile)->ID3v2Tag(true);

        if (!mImageData.empty()) {
            TagLib::ID3v2::AttachedPictureFrame *frame = new TagLib::ID3v2::AttachedPictureFrame;
            frame->setMimeType(mimeType(mImageData));
            frame->setPicture(imageByteVector);
            dynamic_cast<TagLib::ID3v2::Tag *>(tag)->addFrame(frame);
        }
    } else if (mFormat == NeteaseCrypt::FLAC) {
        audioFile = new TagLib::FLAC::File(&audioStream);
        tag = audioFile->tag();

        if (!mImageData.empty()) {
            TagLib::FLAC::Picture *cover = new TagLib::FLAC::Picture;
            cover->setMimeType(mimeType(mImageData));
            cover->setType(TagLib::FLAC::Picture::FrontCover);
            cover->setData(imageByteVector);
            dynamic_cast<TagLib::FLAC::File *>(audioFile)->addPicture(cover);
        }
    } else {
        std::cout << BOLDYELLOW << "[Warn] " << RESET << "Unsupported audio format for metadata fixing." << std::endl;
        if (audioFile) delete audioFile;
        return;
    }

    if (mMetaData != nullptr && tag != nullptr) {
        tag->setTitle(TagLib::String(mMetaData->name(), TagLib::String::UTF8));
        tag->setArtist(TagLib::String(mMetaData->artist(), TagLib::String::UTF8));
        tag->setAlbum(TagLib::String(mMetaData->album(), TagLib::String::UTF8));
    }

    audioFile->save();

    TagLib::ByteVector* modifiedAudioByteVectorPtr = audioStream.data();
    if (modifiedAudioByteVectorPtr != nullptr) {
        mDecryptedAudioData.assign(
            reinterpret_cast<const uint8_t*>(modifiedAudioByteVectorPtr->data()),
            reinterpret_cast<const uint8_t*>(modifiedAudioByteVectorPtr->data() + modifiedAudioByteVectorPtr->size())
        );
    } else {
        std::cout << BOLDYELLOW << "[Warn] " << RESET << "Failed to retrieve modified audio data from TagLib stream." << std::endl;
    }

    delete audioFile; // Clean up
    audioFile = nullptr;
}


void NeteaseCrypt::DumpToMemory() {
    mDecryptedAudioData.clear();
    size_t encrypted_audio_start_pos = mReadPos;
    size_t encrypted_audio_size = mInputBuffer.size() - encrypted_audio_start_pos;

    if (encrypted_audio_size <= 0) {
        std::cout << BOLDYELLOW << "[Warn] " << RESET << "No encrypted audio data found to decrypt." << std::endl;
        return;
    }

    std::vector<unsigned char> buffer(0x8000);
    mDecryptedAudioData.reserve(encrypted_audio_size);

    size_t current_encrypted_offset = encrypted_audio_start_pos;
    size_t bytes_decrypted_total = 0;

    while (current_encrypted_offset < mInputBuffer.size()) {
        size_t bytes_remaining = mInputBuffer.size() - current_encrypted_offset;
        size_t bytes_to_read = std::min((size_t)buffer.size(), bytes_remaining);

        if (bytes_to_read == 0) break;

        if (current_encrypted_offset + bytes_to_read > mInputBuffer.size()) {
            throw std::runtime_error("DumpToMemory internal bounds check failed.");
        }

        std::copy(mInputBuffer.begin() + current_encrypted_offset,
                  mInputBuffer.begin() + current_encrypted_offset + bytes_to_read,
                  buffer.begin());

        for (int i = 0; i < bytes_to_read; i++) {
            int j = (i + 1) & 0xff;
            buffer[i] ^= mKeyBox[(mKeyBox[j] + mKeyBox[(mKeyBox[j] + j) & 0xff]) & 0xff];
        }

        if (mDecryptedAudioData.empty()) {
            if (bytes_to_read >= 3 && buffer[0] == 0x49 && buffer[1] == 0x44 && buffer[2] == 0x33) {
                mFormat = NeteaseCrypt::MP3;
            } else if (bytes_to_read >= 4 && buffer[0] == 0x66 && buffer[1] == 0x4C && buffer[2] == 0x61 && buffer[3] == 0x43) {
                mFormat = NeteaseCrypt::FLAC;
            } else {
                mFormat = NeteaseCrypt::MP3;
                std::cout << BOLDYELLOW << "[Warn] " << RESET << "Could not determine audio format for in-memory data, defaulting to MP3." << std::endl;
            }
        }

        mDecryptedAudioData.insert(mDecryptedAudioData.end(), buffer.begin(), buffer.begin() + bytes_to_read);
        bytes_decrypted_total += bytes_to_read;
        current_encrypted_offset += bytes_to_read;
    }
}


NeteaseCrypt::NeteaseCrypt(const uint8_t* data, size_t size)
    : mInputBuffer(data, data + size), mReadPos(0), mMetaData(nullptr) {

    if (!isNcmFileInMemory()) {
        throw std::invalid_argument("Not netease protected file (invalid magic header).");
    }
    if (!seekInBuffer(8, std::ios_base::cur)) {
        throw std::runtime_error("Failed to seek past NCM magic header.");
    }

    if (!seekInBuffer(2, std::ios_base::cur)) {
        throw std::invalid_argument("Can't seek past 2-byte unknown field.");
    }

    unsigned int n_key_len;
    readFromBuffer(reinterpret_cast<char *>(&n_key_len), sizeof(n_key_len));

    if (n_key_len <= 0) {
        throw std::invalid_argument("Broken NCM file: Non-positive key length.");
    }

    std::vector<char> keydata_raw_encrypted(n_key_len);
    readFromBuffer(keydata_raw_encrypted.data(), n_key_len);

    for (size_t i = 0; i < n_key_len; i++) {
        keydata_raw_encrypted[i] ^= 0x64;
    }

    std::string rawKeyDataStr(keydata_raw_encrypted.begin(), keydata_raw_encrypted.end());
    std::string mKeyDataStr;
    aesEcbDecrypt(sCoreKey, rawKeyDataStr, mKeyDataStr);

    if (mKeyDataStr.length() <= 17) {
        throw std::runtime_error("Decrypted key data is too short for key box derivation.");
    }
    buildKeyBox(reinterpret_cast<const unsigned char*>(mKeyDataStr.c_str() + 17), mKeyDataStr.length() - 17);

    unsigned int n_meta_len;
    readFromBuffer(reinterpret_cast<char *>(&n_meta_len), sizeof(n_meta_len));

    if (n_meta_len <= 0) {
        mMetaData = nullptr;
    } else {
        std::vector<char> modifyData(n_meta_len);
        readFromBuffer(modifyData.data(), n_meta_len);

        for (size_t i = 0; i < n_meta_len; i++) {
            modifyData[i] ^= 0x63;
        }

        std::string swapModifyData;
        if (modifyData.size() < 22) {
             throw std::runtime_error("Metadata too short to skip prefix.");
        }
        swapModifyData = std::string(modifyData.begin() + 22, modifyData.end());

        std::string modifyOutData;
        Base64::Decode(swapModifyData, modifyOutData);

        std::string modifyDecryptData;
        aesEcbDecrypt(sModifyKey, modifyOutData, modifyDecryptData);

        if (modifyDecryptData.length() < 6) {
            throw std::runtime_error("Decrypted metadata too short to skip 'music:' prefix.");
        }
        modifyDecryptData = std::string(modifyDecryptData.begin() + 6, modifyDecryptData.end());

        mMetaData = new NeteaseMusicMetadata(modifyDecryptData);
    }

    if (!seekInBuffer(5, std::ios_base::cur)) {
        throw std::invalid_argument("Can't seek past CRC32 and image version.");
    }

    unsigned int cover_frame_len;
    readFromBuffer(reinterpret_cast<char *>(&cover_frame_len), 4);

    unsigned int n_image_len;
    readFromBuffer(reinterpret_cast<char *>(&n_image_len), sizeof(n_image_len));

    if (n_image_len > 0) {
        mImageData.resize(n_image_len);
        readFromBuffer(&mImageData[0], n_image_len);
    } else {
        std::cout << BOLDYELLOW << "[Warn] " << RESET << "Missing album image information, can't fix album image!" << std::endl;
        mImageData.clear();
    }

    long long bytes_to_skip_after_image_data = cover_frame_len - n_image_len;
    if (bytes_to_skip_after_image_data < 0 || !seekInBuffer(bytes_to_skip_after_image_data, std::ios_base::cur)) {
        std::cout << BOLDYELLOW << "[Warn] " << RESET << "Problem with final image seek (cover_frame_len - n_image_len). Skipping final seek." << std::endl;
    } else {
    }

}

NeteaseCrypt::~NeteaseCrypt() {
    if (mMetaData != nullptr) {
        delete mMetaData;
        mMetaData = nullptr;
    }
}