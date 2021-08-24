#ifndef PES_EXTRACTOR_H
#define PES_EXTRACTOR_H

#ifdef __cplusplus
extern "C" {
#endif

/* Your callback us trigger and you the developer own the lifespan of the 'pes' object.
 * Make sure you call ltn_pes_packet_free(pes) when you're done with it, else leak.
 */
typedef void (*pes_extractor_callback)(void *userContext, struct ltn_pes_packet_s *pes);

int     ltntstools_pes_extractor_alloc(void **hdl, uint16_t pid, uint8_t streamId,
	pes_extractor_callback cb, void *userContext);
void    ltntstools_pes_extractor_free(void *hdl);
ssize_t ltntstools_pes_extractor_write(void *hdl, const uint8_t *pkts, int packetCount);

int     ltntstools_pes_extractor_set_skip_data(void *hdl, int tf);

#ifdef __cplusplus
};
#endif

#endif /* PES_EXTRACTOR_H */
