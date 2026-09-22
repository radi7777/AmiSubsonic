/* AmiSubsonic - Groesse des SBR-Zustands von Helix, siehe aacsize.c. */

#include "sbr.h"

long aac_sbr_size(void)
{
    return (long)sizeof(PSInfoSBR);
}
