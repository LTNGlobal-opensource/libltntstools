#ifndef NALS_H
#define NALS_H

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
};
#endif

#endif /* NALS_H */
