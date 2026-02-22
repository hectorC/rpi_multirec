#include "z_Utils.h"

unsigned short calcFTDIChecksum(const unsigned short a_Values[], unsigned char a_ValuesSize) {
    unsigned short checksum = 0xAAAA;
    for (unsigned char i = 0; i < a_ValuesSize; ++i) {
        checksum = a_Values[i] ^ checksum;
        checksum = (unsigned short)(checksum <<1) | (checksum >> 15);
    }
    return checksum;
}

void bytesToShorts(const unsigned char* a_BytesBuffer, unsigned long long a_BytesLenght, unsigned short* a_ShortsBuffer) {
    for (int i = 0; i < a_BytesLenght / 2; ++i) {
        a_ShortsBuffer[i] = a_BytesBuffer[2 * i] << 8 | a_BytesBuffer[2 * i + 1];
    }
}

void shortsToBytes(const unsigned short* a_ShortsBuffer, unsigned long long a_ShortsLenght, unsigned char* a_BytesBuffer) {
    for (int i = 0; i < a_ShortsLenght; ++i) {
        a_BytesBuffer[2 * i] = (a_ShortsBuffer[i] & 0xFF00) >> 8;
        a_BytesBuffer[2 * i + 1] = (a_ShortsBuffer[i] & 0x00FF);
    }
}
