/*
 * Generic converter template for a selected ASN.1 type.
 * Copyright (c) 2005-2017 Lev Walkin <vlm@lionet.info>.
 * All rights reserved.
 * 
 * To compile with your own ASN.1 type, please redefine the PDU as shown:
 * 
 * cc -DPDU=MyCustomType -o myDecoder.o -c converter-sample.c
 */
#ifdef    HAVE_CONFIG_H
#include <config.h>
#endif
#include <stdio.h>
#include <sys/types.h>
#include <stdlib.h>    /* for atoi(3) */
#include <unistd.h>    /* for getopt(3) */
#include <string.h>    /* for strerror(3) */
#include <sysexits.h>    /* for EX_* exit codes */
#include <errno.h>    /* for errno */

#include <asn_application.h>
#include <asn_internal.h>    /* for ASN__DEFAULT_STACK_MAX */

/* Convert "Type" defined by -DPDU into "asn_DEF_Type" */

#ifndef NO_ASN_PDU
#ifndef PDU
#error Define -DPDU to compile this sample converter.
#endif
#ifdef    ASN_PDU_COLLECTION        /* Generated by asn1c: -pdu=... */
extern asn_TYPE_descriptor_t *asn_pdu_collection[];
#endif
#define    ASN_DEF_PDU(t)    asn_DEF_ ## t
#define    DEF_PDU_Type(t)    ASN_DEF_PDU(t)
#define    PDU_Type    DEF_PDU_Type(PDU)

extern asn_TYPE_descriptor_t PDU_Type;    /* ASN.1 type to be decoded */
#define PDU_Type_Ptr (&PDU_Type)
#else   /* NO_ASN_PDU */
#define PDU_Type_Ptr    NULL
#endif  /* NO_ASN_PDU */

/*
 * Open file and parse its contens.
 */
static void *data_decode_from_file(enum asn_transfer_syntax,
                                   asn_TYPE_descriptor_t *asnTypeOfPDU,
                                   FILE *file, const char *name,
                                   ssize_t suggested_bufsize, int first_pdu);
static int write_out(const void *buffer, size_t size, void *key);
static FILE *argument_to_file(char *av[], int idx);
static char *argument_to_name(char *av[], int idx);

       int opt_debug;   /* -d (or -dd) */
static int opt_check;   /* -c (constraints checking) */
static int opt_stack;   /* -s (maximum stack size) */
static int opt_nopad;   /* -per-nopad (PER input is not padded between msgs) */
static int opt_onepdu;  /* -1 (decode single PDU) */

#ifdef    JUNKTEST        /* Enable -J <probability> */
#define    JUNKOPT    "J:"
static double opt_jprob;    /* Junk bit probability */
static int    junk_failures;
static void   junk_bytes_with_probability(uint8_t *, size_t, double prob);
#else
#define    JUNKOPT
#endif

/* Debug output function */
static void
DEBUG(const char *fmt, ...) {
    va_list ap;
    if(!opt_debug) return;
    fprintf(stderr, "AD: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

static const char *
ats_simple_name(enum asn_transfer_syntax syntax) {
    switch(syntax) {
    case ATS_INVALID:
        return "/dev/null";
    case ATS_NONSTANDARD_PLAINTEXT:
        return "plaintext";
    case ATS_BER:
        return "BER";
    case ATS_DER:
        return "DER";
    case ATS_CER:
        return "CER";
    case ATS_BASIC_OER:
    case ATS_CANONICAL_OER:
        return "OER";
    case ATS_BASIC_XER:
    case ATS_CANONICAL_XER:
        return "XER";
    case ATS_UNALIGNED_BASIC_PER:
    case ATS_UNALIGNED_CANONICAL_PER:
        return "PER";
    default:
        return "<?>";
    }
}


#define OP_OFFSET(fname)  (unsigned)(&(((asn_TYPE_operation_t *)0)->fname))
typedef struct {
    const char *name;
    enum asn_transfer_syntax syntax;
    unsigned codec_offset;
    const char *full_name;
} syntax_selector;

static syntax_selector input_encodings[] = {
    {"ber", ATS_BER, OP_OFFSET(ber_decoder),
     "Input is in BER (Basic Encoding Rules) or DER"},
    {"oer", ATS_BASIC_OER, OP_OFFSET(oer_decoder),
     "Input is in OER (Octet Encoding Rules)"},
    {"per", ATS_UNALIGNED_BASIC_PER, OP_OFFSET(uper_decoder),
     "Input is in Unaligned PER (Packed Encoding Rules)"},
    {"xer", ATS_BASIC_XER, OP_OFFSET(xer_decoder),
     "Input is in XER (XML Encoding Rules)"},
    {0, ATS_INVALID, 0, 0}};

static syntax_selector output_encodings[] = {
    {"der", ATS_DER, OP_OFFSET(der_encoder),
     "Output as DER (Distinguished Encoding Rules)"},
    {"oer", ATS_CANONICAL_OER, OP_OFFSET(oer_encoder),
     "Output as Canonical OER (Octet Encoding Rules)"},
    {"per", ATS_UNALIGNED_CANONICAL_PER, OP_OFFSET(uper_encoder),
     "Output as Unaligned PER (Packed Encoding Rules)"},
    {"xer", ATS_BASIC_XER, OP_OFFSET(xer_encoder),
     "Output as XER (XML Encoding Rules)"},
    {"text", ATS_NONSTANDARD_PLAINTEXT, OP_OFFSET(print_struct),
     "Output as plain semi-structured text"},
    {"null", ATS_INVALID, OP_OFFSET(print_struct),
     "Verify (decode) input, but do not output"},
    {0, ATS_INVALID, 0, 0}};

/*
 * Select ASN.1 Transfer Enocoding Syntax by command line name.
 */
static const syntax_selector *
ats_by_name(const char *name, const asn_TYPE_descriptor_t *td,
            const syntax_selector *first_element) {
    const syntax_selector *element;
    for(element = first_element; element->name; element++) {
        if(strcmp(element->name, name) == 0) {
            if(td && td->op
               && *(const void *const *)(const void *)((const char *)td->op
                                                       + element
                                                             ->codec_offset)) {
                return element;
            }
        }
    }
    return NULL;
}

int
main(int ac, char *av[]) {
    FILE *binary_out;
    static asn_TYPE_descriptor_t *pduType = PDU_Type_Ptr;
    ssize_t suggested_bufsize = 8192;  /* close or equal to stdio buffer */
    int number_of_iterations = 1;
    int num;
    int ch;
    const syntax_selector *sel;
    enum asn_transfer_syntax isyntax = ATS_INVALID;
    enum asn_transfer_syntax osyntax = ATS_BASIC_XER;

#ifndef  PDU
    if(!pduType) {
        fprintf(stderr, "No -DPDU defined during compilation.\n");
#ifdef  NO_ASN_PDU
        exit(0);
#else
        exit(EX_SOFTWARE);
#endif
    }
#endif

    /* Figure out if specialty decoder needs to be default */
    if(ats_by_name("oer", pduType, input_encodings))
        isyntax = ATS_BASIC_OER;
    if(ats_by_name("per", pduType, input_encodings))
        isyntax = ATS_UNALIGNED_BASIC_PER;

    /*
     * Pocess the command-line argments.
     */
    while((ch = getopt(ac, av, "i:o:1b:cdn:p:hs:" JUNKOPT)) != -1)
    switch(ch) {
    case 'i':
        sel = ats_by_name(optarg, pduType, input_encodings);
        if(sel) {
            isyntax = sel->syntax;
        } else {
            fprintf(stderr, "-i<format>: '%s': improper format selector\n",
                    optarg);
            exit(EX_UNAVAILABLE);
        }
        break;
    case 'o':
        sel = ats_by_name(optarg, pduType, output_encodings);
        if(sel) {
            osyntax = sel->syntax;
        } else {
            fprintf(stderr, "-o<format>: '%s': improper format selector\n",
                    optarg);
            exit(EX_UNAVAILABLE);
        }
        break;
    case '1':
        opt_onepdu = 1;
        break;
    case 'b':
        suggested_bufsize = atoi(optarg);
        if(suggested_bufsize < 1
            || suggested_bufsize > 16 * 1024 * 1024) {
            fprintf(stderr,
                "-b %s: Improper buffer size (1..16M)\n",
                optarg);
            exit(EX_UNAVAILABLE);
        }
        break;
    case 'c':
        opt_check = 1;
        break;
    case 'd':
        opt_debug++;    /* Double -dd means ASN.1 debug */
        break;
    case 'n':
        number_of_iterations = atoi(optarg);
        if(number_of_iterations < 1) {
            fprintf(stderr,
                "-n %s: Improper iterations count\n", optarg);
            exit(EX_UNAVAILABLE);
        }
        break;
    case 'p':
        if(strcmp(optarg, "er-nopad") == 0) {
            opt_nopad = 1;
            break;
        }
#ifdef    ASN_PDU_COLLECTION
        if(strcmp(optarg, "list") == 0) {
            asn_TYPE_descriptor_t **pdu = asn_pdu_collection;
            fprintf(stderr, "Available PDU types:\n");
            for(; *pdu; pdu++) printf("%s\n", (*pdu)->name);
            exit(0);
        } else if(optarg[0] >= 'A' && optarg[0] <= 'Z') {
            asn_TYPE_descriptor_t **pdu = asn_pdu_collection;
            while(*pdu && strcmp((*pdu)->name, optarg)) pdu++;
            if(*pdu) { pduType = *pdu; break; }
            fprintf(stderr, "-p %s: Unrecognized PDU\n", optarg);
            exit(EX_USAGE);
        }
#else   /* Without -pdu=auto there's just a single type */
        if(strcmp(optarg, "list") == 0) {
            fprintf(stderr, "Available PDU types:\n");
            printf("%s\n", pduType->name);
            exit(0);
        } else if(optarg[0] >= 'A' && optarg[0] <= 'Z') {
            if(strcmp(optarg, pduType->name) == 0) {
                break;
            }
            fprintf(stderr, "-p %s: Unrecognized PDU\n", optarg);
            exit(EX_USAGE);
        }
#endif    /* ASN_PDU_COLLECTION */
        fprintf(stderr, "-p %s: Unrecognized option\n", optarg);
        exit(EX_UNAVAILABLE);
    case 's':
        opt_stack = atoi(optarg);
        if(opt_stack < 0) {
            fprintf(stderr,
                "-s %s: Non-negative value expected\n",
                optarg);
            exit(EX_UNAVAILABLE);
        }
        break;
#ifdef    JUNKTEST
    case 'J':
        opt_jprob = strtod(optarg, 0);
        if(opt_jprob <= 0.0 || opt_jprob > 1.0) {
            fprintf(stderr,
                "-J %s: Probability range 0..1 expected \n",
                optarg);
            exit(EX_UNAVAILABLE);
        }
        break;
#endif    /* JUNKTEST */
    case 'h':
    default:
#ifdef    ASN_CONVERTER_TITLE
#define    _AXS(x)    #x
#define    _ASX(x)    _AXS(x)
        fprintf(stderr, "%s\n", _ASX(ASN_CONVERTER_TITLE));
#endif
        fprintf(stderr, "Usage: %s [options] <datafile> ...\n", av[0]);
        fprintf(stderr, "Where options are:\n");
        for(sel = input_encodings; sel->name; sel++) {
            if(ats_by_name(sel->name, pduType, sel)) {
                fprintf(stderr, "  -i%s        %s%s\n", sel->name,
                        sel->full_name,
                        (sel->syntax == isyntax) ? " (DEFAULT)" : "");
            }
        }
        for(sel = output_encodings; sel->name; sel++) {
            if(ats_by_name(sel->name, pduType, sel)) {
                fprintf(stderr, "  -o%s%s       %s%s\n", sel->name,
                        strlen(sel->name) > 3 ? "" : " ",
                        sel->full_name,
                        (sel->syntax == osyntax) ? " (DEFAULT)" : "");
            }
        }
        if(pduType->op->uper_decoder) {
            fprintf(stderr,
                    "  -per-nopad   Assume PER PDUs are not padded (-iper)\n");
        }
#ifdef    ASN_PDU_COLLECTION
        fprintf(stderr,
        "  -p <PDU>     Specify PDU type to decode\n"
        "  -p list      List available PDUs\n");
#endif    /* ASN_PDU_COLLECTION */
        fprintf(stderr,
        "  -1           Decode only the first PDU in file\n"
        "  -b <size>    Set the i/o buffer size (default is %ld)\n"
        "  -c           Check ASN.1 constraints after decoding\n"
        "  -d           Enable debugging (-dd is even better)\n"
        "  -n <num>     Process files <num> times\n"
        "  -s <size>    Set the stack usage limit (default is %d)\n"
#ifdef    JUNKTEST
        "  -J <prob>    Set random junk test bit garbaging probability\n"
#endif
        , (long)suggested_bufsize, ASN__DEFAULT_STACK_MAX);
        exit(EX_USAGE);
    }

    ac -= optind;
    av += optind;

    if(ac < 1) {
        fprintf(stderr, "%s: No input files specified. "
                "Try '-h' for more information\n",
                av[-optind]);
        exit(EX_USAGE);
    }

    if(isatty(1)) {
        const int is_text_output = osyntax == ATS_NONSTANDARD_PLAINTEXT
                                   || osyntax == ATS_BASIC_XER
                                   || osyntax == ATS_CANONICAL_XER;
        if(is_text_output) {
            binary_out = stdout;
        } else {
            fprintf(stderr, "(Suppressing binary output to a terminal.)\n");
            binary_out = fopen("/dev/null", "wb");
            if(!binary_out) {
                fprintf(stderr, "Can't open /dev/null: %s\n", strerror(errno));
                exit(EX_OSERR);
            }
        }
    } else {
        binary_out = stdout;
    }
    setvbuf(stdout, 0, _IOLBF, 0);

    for(num = 0; num < number_of_iterations; num++) {
      int ac_i;
      /*
       * Process all files in turn.
       */
      for(ac_i = 0; ac_i < ac; ac_i++) {
        asn_enc_rval_t erv;
        void *structure;    /* Decoded structure */
        FILE *file = argument_to_file(av, ac_i);
        char *name = argument_to_name(av, ac_i);
        int first_pdu;

        for(first_pdu = 1; file && (first_pdu || !opt_onepdu); first_pdu = 0) {
            /*
             * Decode the encoded structure from file.
             */
            structure = data_decode_from_file(isyntax, pduType, file, name,
                                              suggested_bufsize, first_pdu);
            if(!structure) {
                if(errno) {
                    /* Error message is already printed */
                    exit(EX_DATAERR);
                } else {
                    /* EOF */
                    break;
                }
            }

            /* Check ASN.1 constraints */
            if(opt_check) {
                char errbuf[128];
                size_t errlen = sizeof(errbuf);
                if(asn_check_constraints(pduType, structure, errbuf, &errlen)) {
                    fprintf(stderr,
                            "%s: ASN.1 constraint "
                            "check failed: %s\n",
                            name, errbuf);
                    exit(EX_DATAERR);
                }
            }

            if(osyntax == ATS_INVALID) {
#ifdef JUNKTEST
                if(opt_jprob == 0.0) {
                    fprintf(stderr, "%s: decoded successfully\n", name);
                }
#else
                fprintf(stderr, "%s: decoded successfully\n", name);
#endif
            } else {
                erv = asn_encode(NULL, osyntax, pduType, structure, write_out,
                                 binary_out);

                if(erv.encoded == -1) {
                    fprintf(stderr, "%s: Cannot convert %s into %s\n", name,
                            pduType->name, ats_simple_name(osyntax));
                    exit(EX_UNAVAILABLE);
                }
                DEBUG("Encoded in %zd bytes of %s", erv.encoded,
                      ats_simple_name(osyntax));
            }

            ASN_STRUCT_FREE(*pduType, structure);
        }

        if(file && file != stdin) {
            fclose(file);
        }
      }
    }

#ifdef    JUNKTEST
    if(opt_jprob > 0.0) {
        fprintf(stderr, "Junked %f OK (%d/%d)\n",
            opt_jprob, junk_failures, number_of_iterations);
    }
#endif    /* JUNKTEST */

    return 0;
}

static struct dynamic_buffer {
    uint8_t *data;        /* Pointer to the data bytes */
    size_t offset;        /* Offset from the start */
    size_t length;        /* Length of meaningful contents */
    size_t unbits;        /* Unused bits in the last byte */
    size_t allocated;    /* Allocated memory for data */
    int    nreallocs;    /* Number of data reallocations */
    off_t  bytes_shifted;    /* Number of bytes ever shifted */
} DynamicBuffer;

static void
buffer_dump() {
    uint8_t *p = DynamicBuffer.data + DynamicBuffer.offset;
    uint8_t *e = p + DynamicBuffer.length - (DynamicBuffer.unbits ? 1 : 0);
    if(!opt_debug) return;
    DEBUG("Buffer: { d=%p, o=%ld, l=%ld, u=%ld, a=%ld, s=%ld }",
        DynamicBuffer.data,
        (long)DynamicBuffer.offset,
        (long)DynamicBuffer.length,
        (long)DynamicBuffer.unbits,
        (long)DynamicBuffer.allocated,
        (long)DynamicBuffer.bytes_shifted);
    for(; p < e; p++) {
        fprintf(stderr, " %c%c%c%c%c%c%c%c",
            ((*p >> 7) & 1) ? '1' : '0',
            ((*p >> 6) & 1) ? '1' : '0',
            ((*p >> 5) & 1) ? '1' : '0',
            ((*p >> 4) & 1) ? '1' : '0',
            ((*p >> 3) & 1) ? '1' : '0',
            ((*p >> 2) & 1) ? '1' : '0',
            ((*p >> 1) & 1) ? '1' : '0',
            ((*p >> 0) & 1) ? '1' : '0');
    }
    if(DynamicBuffer.unbits) {
        unsigned int shift;
        fprintf(stderr, " ");
        for(shift = 7; shift >= DynamicBuffer.unbits; shift--)
            fprintf(stderr, "%c", ((*p >> shift) & 1) ? '1' : '0');
        fprintf(stderr, " %ld:%ld\n",
            (long)DynamicBuffer.length - 1,
            (long)8 - DynamicBuffer.unbits);
    } else {
        fprintf(stderr, " %ld\n", (long)DynamicBuffer.length);
    }
}

/*
 * Move the buffer content left N bits, possibly joining it with
 * preceeding content.
 */
static void
buffer_shift_left(size_t offset, int bits) {
    uint8_t *ptr = DynamicBuffer.data + DynamicBuffer.offset + offset;
    uint8_t *end = DynamicBuffer.data + DynamicBuffer.offset
            + DynamicBuffer.length - 1;
    
    if(!bits) return;

    DEBUG("Shifting left %d bits off %ld (o=%ld, u=%ld, l=%ld)",
        bits, (long)offset,
        (long)DynamicBuffer.offset,
        (long)DynamicBuffer.unbits,
        (long)DynamicBuffer.length);

    if(offset) {
        int right;
        right = ptr[0] >> (8 - bits);

        DEBUG("oleft: %c%c%c%c%c%c%c%c",
            ((ptr[-1] >> 7) & 1) ? '1' : '0',
            ((ptr[-1] >> 6) & 1) ? '1' : '0',
            ((ptr[-1] >> 5) & 1) ? '1' : '0',
            ((ptr[-1] >> 4) & 1) ? '1' : '0',
            ((ptr[-1] >> 3) & 1) ? '1' : '0',
            ((ptr[-1] >> 2) & 1) ? '1' : '0',
            ((ptr[-1] >> 1) & 1) ? '1' : '0',
            ((ptr[-1] >> 0) & 1) ? '1' : '0');

        DEBUG("oriht: %c%c%c%c%c%c%c%c",
            ((ptr[0] >> 7) & 1) ? '1' : '0',
            ((ptr[0] >> 6) & 1) ? '1' : '0',
            ((ptr[0] >> 5) & 1) ? '1' : '0',
            ((ptr[0] >> 4) & 1) ? '1' : '0',
            ((ptr[0] >> 3) & 1) ? '1' : '0',
            ((ptr[0] >> 2) & 1) ? '1' : '0',
            ((ptr[0] >> 1) & 1) ? '1' : '0',
            ((ptr[0] >> 0) & 1) ? '1' : '0');

        DEBUG("mriht: %c%c%c%c%c%c%c%c",
            ((right >> 7) & 1) ? '1' : '0',
            ((right >> 6) & 1) ? '1' : '0',
            ((right >> 5) & 1) ? '1' : '0',
            ((right >> 4) & 1) ? '1' : '0',
            ((right >> 3) & 1) ? '1' : '0',
            ((right >> 2) & 1) ? '1' : '0',
            ((right >> 1) & 1) ? '1' : '0',
            ((right >> 0) & 1) ? '1' : '0');

        ptr[-1] = (ptr[-1] & (0xff << bits)) | right;

        DEBUG("after: %c%c%c%c%c%c%c%c",
            ((ptr[-1] >> 7) & 1) ? '1' : '0',
            ((ptr[-1] >> 6) & 1) ? '1' : '0',
            ((ptr[-1] >> 5) & 1) ? '1' : '0',
            ((ptr[-1] >> 4) & 1) ? '1' : '0',
            ((ptr[-1] >> 3) & 1) ? '1' : '0',
            ((ptr[-1] >> 2) & 1) ? '1' : '0',
            ((ptr[-1] >> 1) & 1) ? '1' : '0',
            ((ptr[-1] >> 0) & 1) ? '1' : '0');
    }

    buffer_dump();

    for(; ptr < end; ptr++) {
        int right = ptr[1] >> (8 - bits);
        *ptr = (*ptr << bits) | right;
    }
    *ptr <<= bits;

    DEBUG("Unbits [%d=>", (int)DynamicBuffer.unbits);
    if(DynamicBuffer.unbits == 0) {
        DynamicBuffer.unbits += bits;
    } else {
        DynamicBuffer.unbits += bits;
        if(DynamicBuffer.unbits > 7) {
            DynamicBuffer.unbits -= 8;
            DynamicBuffer.length--;
            DynamicBuffer.bytes_shifted++;
        }
    }
    DEBUG("Unbits =>%d]", (int)DynamicBuffer.unbits);

    buffer_dump();

    DEBUG("Shifted. Now (o=%ld, u=%ld l=%ld)",
        (long)DynamicBuffer.offset,
        (long)DynamicBuffer.unbits,
        (long)DynamicBuffer.length);
    

}

/*
 * Ensure that the buffer contains at least this amount of free space.
 */
static void add_bytes_to_buffer(const void *data2add, size_t bytes) {

    if(bytes == 0) return;

    DEBUG("=> add_bytes(%ld) { o=%ld l=%ld u=%ld, s=%ld }",
        (long)bytes,
        (long)DynamicBuffer.offset,
        (long)DynamicBuffer.length,
        (long)DynamicBuffer.unbits,
        (long)DynamicBuffer.allocated);

    if(DynamicBuffer.allocated
    >= (DynamicBuffer.offset + DynamicBuffer.length + bytes)) {
        DEBUG("\tNo buffer reallocation is necessary");
    } else if(bytes <= DynamicBuffer.offset) {
        DEBUG("\tContents shifted by %ld", DynamicBuffer.offset);

        /* Shift the buffer contents */
        memmove(DynamicBuffer.data,
                DynamicBuffer.data + DynamicBuffer.offset,
            DynamicBuffer.length);
        DynamicBuffer.bytes_shifted += DynamicBuffer.offset;
        DynamicBuffer.offset = 0;
    } else {
        size_t newsize = (DynamicBuffer.allocated << 2) + bytes;
        void *p = MALLOC(newsize);
        if(!p) {
            perror("malloc()");
            exit(EX_OSERR);
        }
        memcpy(p,
            DynamicBuffer.data + DynamicBuffer.offset,
            DynamicBuffer.length);
        FREEMEM(DynamicBuffer.data);
        DynamicBuffer.data = (uint8_t *)p;
        DynamicBuffer.offset = 0;
        DynamicBuffer.allocated = newsize;
        DynamicBuffer.nreallocs++;
        DEBUG("\tBuffer reallocated to %ld (%d time)",
            newsize, DynamicBuffer.nreallocs);
    }

    memcpy(DynamicBuffer.data
        + DynamicBuffer.offset + DynamicBuffer.length,
        data2add, bytes);
    DynamicBuffer.length += bytes;
    if(DynamicBuffer.unbits) {
        int bits = DynamicBuffer.unbits;
        DynamicBuffer.unbits = 0;
        buffer_shift_left(DynamicBuffer.length - bytes, bits);
    }

    DEBUG("<= add_bytes(%ld) { o=%ld l=%ld u=%ld, s=%ld }",
        (long)bytes,
        (long)DynamicBuffer.offset,
        (long)DynamicBuffer.length,
        (long)DynamicBuffer.unbits,
        (long)DynamicBuffer.allocated);
}

static int
is_syntax_PER(enum asn_transfer_syntax syntax) {
    return (syntax != ATS_UNALIGNED_BASIC_PER
            && syntax != ATS_UNALIGNED_CANONICAL_PER);
}

static int
restartability_supported(enum asn_transfer_syntax syntax) {
    return is_syntax_PER(syntax);
}

static void *
data_decode_from_file(enum asn_transfer_syntax isyntax, asn_TYPE_descriptor_t *pduType, FILE *file, const char *name, ssize_t suggested_bufsize, int on_first_pdu) {
    static uint8_t *fbuf;
    static ssize_t fbuf_size;
    static asn_codec_ctx_t s_codec_ctx;
    asn_codec_ctx_t *opt_codec_ctx = 0;
    void *structure = 0;
    asn_dec_rval_t rval;
    size_t old_offset;    
    size_t new_offset;
    int tolerate_eof;
    size_t rd;

    if(!file) {
        fprintf(stderr, "%s: %s\n", name, strerror(errno));
        errno = EINVAL;
        return 0;
    }

    if(opt_stack) {
        s_codec_ctx.max_stack_size = opt_stack;
        opt_codec_ctx = &s_codec_ctx;
    }

    DEBUG("Processing %s", name);

    /* prepare the file buffer */
    if(fbuf_size != suggested_bufsize) {
        fbuf = (uint8_t *)REALLOC(fbuf, suggested_bufsize);
        if(!fbuf) {
            perror("realloc()");
            exit(EX_OSERR);
        }
        fbuf_size = suggested_bufsize;
    }

    if(on_first_pdu) {
        DynamicBuffer.offset = 0;
        DynamicBuffer.length = 0;
        DynamicBuffer.unbits = 0;
        DynamicBuffer.allocated = 0;
        DynamicBuffer.bytes_shifted = 0;
        DynamicBuffer.nreallocs = 0;
    }

    old_offset = DynamicBuffer.bytes_shifted + DynamicBuffer.offset;

    /* Pretend immediate EOF */
    rval.code = RC_WMORE;
    rval.consumed = 0;

    for(tolerate_eof = 1;    /* Allow EOF first time buffer is non-empty */
        (rd = fread(fbuf, 1, fbuf_size, file))
        || feof(file) == 0
        || (tolerate_eof && DynamicBuffer.length)
        ;) {
        int      ecbits = 0;    /* Extra consumed bits in case of PER */
        uint8_t *i_bptr;
        size_t   i_size;

        /*
         * Copy the data over, or use the original buffer.
         */
        if(DynamicBuffer.allocated) {
            /* Append new data into the existing dynamic buffer */
            add_bytes_to_buffer(fbuf, rd);
            i_bptr = DynamicBuffer.data + DynamicBuffer.offset;
            i_size = DynamicBuffer.length;
        } else {
            i_bptr = fbuf;
            i_size = rd;
        }

        DEBUG("Decoding %ld bytes", (long)i_size);

#ifdef    JUNKTEST
        junk_bytes_with_probability(i_bptr, i_size, opt_jprob);
#endif

        if(is_syntax_PER(isyntax) && opt_nopad) {
#ifdef  ASN_DISABLE_PER_SUPPORT
            rval.code = RC_FAIL;
            rval.consumed = 0;
#else
            rval = uper_decode(opt_codec_ctx, pduType, (void **)&structure,
                               i_bptr, i_size, 0, DynamicBuffer.unbits);
            /* uper_decode() returns bits! */
            ecbits = rval.consumed % 8; /* Bits consumed from the last byte */
            rval.consumed >>= 3;    /* Convert bits into bytes. */
#endif
            /* Non-padded PER decoder */
        } else {
            rval = asn_decode(opt_codec_ctx, isyntax, pduType,
                              (void **)&structure, i_bptr, i_size);
        }
        if(rval.code == RC_WMORE && !restartability_supported(isyntax)) {
            /* PER does not support restartability */
            ASN_STRUCT_FREE(*pduType, structure);
            structure = 0;
            rval.consumed = 0;
            /* Continue accumulating data */
        }

        DEBUG("decode(%ld) consumed %ld+%db (%ld), code %d",
            (long)DynamicBuffer.length,
            (long)rval.consumed, ecbits, (long)i_size,
            rval.code);

        if(DynamicBuffer.allocated == 0) {
            /*
             * Flush remainder into the intermediate buffer.
             */
            if(rval.code != RC_FAIL && rval.consumed < rd) {
                add_bytes_to_buffer(fbuf + rval.consumed,
                    rd - rval.consumed);
                buffer_shift_left(0, ecbits);
                DynamicBuffer.bytes_shifted = rval.consumed;
                rval.consumed = 0;
                ecbits = 0;
            }
        }

        /*
         * Adjust position inside the source buffer.
         */
        if(DynamicBuffer.allocated) {
            DynamicBuffer.offset += rval.consumed;
            DynamicBuffer.length -= rval.consumed;
        } else {
            DynamicBuffer.bytes_shifted += rval.consumed;
        }

        switch(rval.code) {
        case RC_OK:
            if(ecbits) buffer_shift_left(0, ecbits);
            DEBUG("RC_OK, finishing up with %ld+%d",
                (long)rval.consumed, ecbits);
            return structure;
        case RC_WMORE:
            DEBUG("RC_WMORE, continuing read=%ld, cons=%ld "
                " with %ld..%ld-%ld..%ld",
                (long)rd,
                (long)rval.consumed,
                (long)DynamicBuffer.offset,
                (long)DynamicBuffer.length,
                (long)DynamicBuffer.unbits,
                (long)DynamicBuffer.allocated);
            if(!rd) tolerate_eof--;
            continue;
        case RC_FAIL:
            break;
        }
        break;
    }

    DEBUG("Clean up partially decoded %s", pduType->name);
    ASN_STRUCT_FREE(*pduType, structure);

    new_offset = DynamicBuffer.bytes_shifted + DynamicBuffer.offset;

    /*
     * Print a message and return failure only if not EOF,
     * unless this is our first PDU (empty file).
     */
    if(on_first_pdu
    || DynamicBuffer.length
    || new_offset - old_offset > ((isyntax == ATS_BASIC_XER)?sizeof("\r\n")-1:0)
    ) {

#ifdef    JUNKTEST
        /*
         * Nothing's wrong with being unable to decode junk.
         * Simulate EOF.
         */
        if(opt_jprob != 0.0) {
            junk_failures++;
            errno = 0;
            return 0;
        }
#endif

        DEBUG("ofp %d, no=%ld, oo=%ld, dbl=%ld",
            on_first_pdu, (long)new_offset, (long)old_offset,
            (long)DynamicBuffer.length);
        fprintf(stderr, "%s: "
            "Decode failed past byte %ld: %s\n",
            name, (long)new_offset,
            (rval.code == RC_WMORE)
                ? "Unexpected end of input"
                : "Input processing error");
#ifndef    ENOMSG
#define    ENOMSG EINVAL
#endif
#ifndef    EBADMSG
#define    EBADMSG EINVAL
#endif
        errno = (rval.code == RC_WMORE) ? ENOMSG : EBADMSG;
    } else {
        /* Got EOF after a few successful PDUs */
        errno = 0;
    }

    return 0;
}

/* Dump the buffer out to the specified FILE */
static int write_out(const void *buffer, size_t size, void *key) {
    FILE *fp = (FILE *)key;
    return (fwrite(buffer, 1, size, fp) == size) ? 0 : -1;
}

static int argument_is_stdin(char *av[], int idx) {
    if(strcmp(av[idx], "-")) {
        return 0; /* Certainly not <stdin> */
    } else {
        /* This might be <stdin>, unless `./program -- -` */
        if(strcmp(av[-1], "--"))
            return 1;
        else
            return 0;
    }
}

static FILE *argument_to_file(char *av[], int idx) {
    return argument_is_stdin(av, idx) ? stdin : fopen(av[idx], "rb");
}

static char *argument_to_name(char *av[], int idx) {
    return argument_is_stdin(av, idx) ? "standard input" : av[idx];
}

#ifdef    JUNKTEST
/*
 * Fill bytes with some garbage with specified probability (more or less).
 */
static void
junk_bytes_with_probability(uint8_t *buf, size_t size, double prob) {
    static int junkmode;
    uint8_t *ptr;
    uint8_t *end;
    if(opt_jprob <= 0.0) return;
    for(ptr = buf, end = ptr + size; ptr < end; ptr++) {
        int byte = *ptr;
        if(junkmode++ & 1) {
            if((((double)random() / RAND_MAX) < prob))
                byte = random() & 0xff;
        } else {
#define    BPROB(b)    ((((double)random() / RAND_MAX) < prob) ? b : 0)
            byte ^= BPROB(0x80);
            byte ^= BPROB(0x40);
            byte ^= BPROB(0x20);
            byte ^= BPROB(0x10);
            byte ^= BPROB(0x08);
            byte ^= BPROB(0x04);
            byte ^= BPROB(0x02);
            byte ^= BPROB(0x01);
        }
        if(byte != *ptr) {
            DEBUG("Junk buf[%d] %02x -> %02x", ptr - buf, *ptr, byte);
            *ptr = byte;
        }
    }
}
#endif    /* JUNKTEST */

