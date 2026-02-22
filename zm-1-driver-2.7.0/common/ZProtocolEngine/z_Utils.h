



#ifndef Z_UTILS_H
#define Z_UTILS_H


unsigned short calcFTDIChecksum(const unsigned short a_Values[], unsigned char a_ValuesSize);

void bytesToShorts(const unsigned char* a_BytesBuffer, unsigned long long a_BytesLenght, unsigned short* a_ShortsBuffer);

void shortsToBytes(const unsigned short* a_ShortsBuffer, unsigned long long a_ShortsLenght, unsigned char* a_BytesBuffer);

#endif 
