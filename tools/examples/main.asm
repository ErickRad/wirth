.EXTERN VALUE
.ENTRY start

MACRO BUILD_OP reg
    MACRO INNER_PRINT r
        PRINT {r}
    MEND
    LOADI R7, 2
    MUL {reg}, R7
    INNER_PRINT {reg}
MEND

start:
    LOAD R0, VALUE
    BUILD_OP R0
    HALT

