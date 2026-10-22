#ifndef NEODAX_SVG_H
#define NEODAX_SVG_H

#include "neodax_bridge.h"

/* SVG output generation */
int neodax_generate_svg(integrated_binary_t *ibin,
                       static_analysis_opts_t *opts,
                       const char *filename);

#endif /* NEODAX_SVG_H */
