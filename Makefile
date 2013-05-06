OBJ := iomalloc.o
TARGET := iomalloc

CFLAGS := -Wall -Wextra -pipe -Wwrite-strings -Wsign-compare \
					-Wshadow -Wundef -Wstrict-prototypes   \
					-fno-strict-aliasing -fno-common  \
					-Winit-self -Wredundant-decls \
					-Wstrict-aliasing=3 -Wswitch-enum \
					-Wno-system-headers -Wundef -Wvolatile-register-var \
					-Wcast-align -Wbad-function-cast -Wwrite-strings \
					-Wold-style-definition  -Wdeclaration-after-statement \
					-fstack-protector -fstrict-overflow -Wstrict-overflow=2

CFLAGS += -ggdb3 -Werror
CFLAGS += -DTEST_BUILD=1

.SUFFIXES:
.SUFFIXES: .c .o

all: $(TARGET)

%.o : %.c
	$(CC) -c $(CFLAGS) $(EXTRA_CFLAGS) $(CPPFLAGS) $< -o $@

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -o $(TARGET) $(OBJ)

clean:
	-rm -f $(OBJ) $(TARGET) core core.*

cscope:
	cscope -R -b
