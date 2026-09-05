#ifndef TEST_FAKE_TKERNEL_H
#define TEST_FAKE_TKERNEL_H

#include <stdint.h>

typedef int32_t ID;
typedef int32_t ER;
typedef int32_t TMO;
typedef uint32_t ATR;
typedef uint32_t UINT;

typedef struct t_cflg
{
    void *exinf;
    ATR flgatr;
    UINT iflgptn;
} T_CFLG;

#define E_OK (0)
#define E_OBJ (-41)
#define E_NOEXS (-42)
#define E_TMOUT (-50)
#define MERCD(error) (error)
#define TMO_POL (0)
#define TMO_FEVR (-1)
#define TA_TFIFO (0U)
#define TWF_ORW (1U)
#define TWF_BITCLR (0x20U)

ID tk_cre_flg(const T_CFLG *config);
ER tk_del_flg(ID flag_id);
ER tk_set_flg(ID flag_id, UINT pattern);
ER tk_wai_flg(ID flag_id, UINT wait_pattern, UINT wait_mode,
    UINT *matched_pattern, TMO timeout);

#endif
