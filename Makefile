# Windows Device Name Manager - MinGW-w64 (gcc) 用 Makefile
#
#   mingw32-make            リリースビルド (build/DeviceNameManager.exe)
#   mingw32-make debug      デバッグ情報つき + コンソール
#   mingw32-make clean

CC      := gcc
WINDRES := windres

SRCDIR  := src
OBJDIR  := build/obj
BINDIR  := build
TARGET  := $(BINDIR)/DeviceNameManager.exe

SRCS := \
	$(SRCDIR)/guids.c \
	$(SRCDIR)/util.c \
	$(SRCDIR)/devlist.c \
	$(SRCDIR)/devops.c \
	$(SRCDIR)/matching.c \
	$(SRCDIR)/safety.c \
	$(SRCDIR)/history.c \
	$(SRCDIR)/ui_dlg.c \
	$(SRCDIR)/ui_main.c

OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))
RES  := $(OBJDIR)/app.res.o

# _WIN32_WINNT=0x0A00 は Windows 10/11 API (DEVPKEY 等) を出すために必要。
# -DUNICODE は -municode も定義するが、依存を明示するため自分でも渡す。
# 無いと ListView_SetItemText 等が A 版に解決され、ワイド文字列を ANSI と
# して扱って表示が壊れる (気づけるよう dnm.h に #error を置いてある)。
CFLAGS := -std=c99 -municode -Wall -Wextra -Wno-unused-parameter \
          -finput-charset=UTF-8 -DUNICODE -D_UNICODE \
          -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000000 \
          -I$(SRCDIR)

# ole32/oleaut32 : COM、propsys : PROPVARIANT、uuid : 既定の GUID
# netshell / newdev はリンクしない。DiUninstallDevice と
# NcIsValidConnectionName は devops.c が実行時に DLL から取る。
LDFLAGS := -municode -mwindows
LDLIBS  := -lsetupapi -lcfgmgr32 -lcomctl32 -lole32 -loleaut32 \
           -lpropsys -luuid -lshell32 -ladvapi32 -luser32 -lgdi32

ifeq ($(MAKECMDGOALS),debug)
CFLAGS  += -g -O0 -DDNM_DEBUG
LDFLAGS := -municode -mconsole
else
CFLAGS  += -O2
endif

UITARGET := $(BINDIR)/DeviceNameManager-uitest.exe
UIRES    := $(OBJDIR)/app-uitest.res.o

.PHONY: all debug clean run uitest

all: $(TARGET)

debug: $(TARGET)

# UAC を出さずに画面だけ確かめるためのビルド。
# 中身は同一で、マニフェストだけ asInvoker。変更操作は権限不足で失敗する。
uitest: $(UITARGET)

$(UITARGET): $(OBJS) $(UIRES) | $(BINDIR)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(UIRES) $(LDLIBS)

$(UIRES): $(SRCDIR)/app.rc $(SRCDIR)/app-uitest.manifest $(SRCDIR)/resource.h | $(OBJDIR)
	$(WINDRES) -I$(SRCDIR) -DDNM_MANIFEST=\"app-uitest.manifest\" -O coff -o $@ $<

$(TARGET): $(OBJS) $(RES) | $(BINDIR)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(RES) $(LDLIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/dnm.h $(SRCDIR)/resource.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# .rc は app.manifest を取り込むので、そちらの更新でも作り直す
$(RES): $(SRCDIR)/app.rc $(SRCDIR)/app.manifest $(SRCDIR)/resource.h | $(OBJDIR)
	$(WINDRES) -I$(SRCDIR) -O coff -o $@ $<

$(BINDIR):
	@mkdir -p $(BINDIR) 2>nul || md "$(subst /,\,$(BINDIR))" 2>nul || exit 0

$(OBJDIR):
	@mkdir -p $(OBJDIR) 2>nul || md "$(subst /,\,$(OBJDIR))" 2>nul || exit 0

clean:
	@-rm -rf build 2>nul || rd /s /q build 2>nul || exit 0

run: $(TARGET)
	$(TARGET)
