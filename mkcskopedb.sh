find  .                                                                \
	-path "./arch/*" ! -path "./arch/i386*" -prune -o               \
	-path "./include/asm-*" ! -path "./include/asm-i386*" -prune -o \
	-path "./tmp*" -prune -o                                           \
	-path "./Documentation*" -prune -o                                 \
	-path "./scripts*" -prune -o                                       \
	-path "./drivers*" -prune -o                                       \
        -name "*.[chxsS]" -print > cscope.files


cscope -b -q -k
