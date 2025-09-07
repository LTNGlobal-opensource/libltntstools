#ifndef NALS_H
#define NALS_H

struct ltn_nal_headers_s
{
    const uint8_t *ptr;
    uint32_t       lengthBytes;
    uint8_t        nalType;
    const char    *nalName;
};


struct ltn_sei_headers_s
{
    const uint8_t *ptr;
    uint32_t       lengthBytes;
    uint8_t        seiType;
    const char    *seiName;
};

#endif /* NALS_H */
