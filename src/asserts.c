#if(USE_CHECKS)
#define \
check(condition,...) assert((condition) __VA_OPT__(&&) __VA_ARGS__)
#define \
panic(condition,...) check(condition __VA_OPT__(,) __VA_ARGS__)

#else
#define \
check(condition,...) ((void)(condition))
#define \
panic(condition,...) ({\
        fprintf(stderr, "panic"__VA_OPT__(": ")__VA_ARGS__"!\n");\
        exit(1);\
})
#endif
