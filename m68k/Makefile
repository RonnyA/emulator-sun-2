# Append .exe on Windows so make can see the binary already exists and
# doesn't relink + regenerate the ops files on every invocation.
ifeq ($(OS),Windows_NT)
    EXE_EXT := .exe
else ifneq (,$(findstring MINGW,$(shell uname -s 2>/dev/null)))
    EXE_EXT := .exe
else ifneq (,$(findstring MSYS,$(shell uname -s 2>/dev/null)))
    EXE_EXT := .exe
else
    EXE_EXT :=
endif

M68KMAKE := m68kmake$(EXE_EXT)

all: m68kops.h $(M68KMAKE)

m68kops.h: $(M68KMAKE)
	./$(M68KMAKE) ../m68k m68k_in.c

$(M68KMAKE): m68kmake.c m68k_in.c m68kcpu.h
	$(CC) -o $@ $<

clean:
	rm -f *.o
	rm -f m68kmake m68kmake.exe
	rm -f m68kopac.c m68kopdm.c m68kopnz.c m68kops.c m68kops.h
