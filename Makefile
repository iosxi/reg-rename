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
	$(SRCDIR)/config.c \
	$(SRCDIR)/backup.c \
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

# MinGW-w64 の spec は実行ファイルに必ず default-manifest.o を足す
#   *startfile: ... %{!shared:%:if-exists(default-manifest.o%s)}
# これは asInvoker のマニフェストを持っていて、app.rc の
# requireAdministrator と衝突する。実測: リンク時に
#   ld: .rsrc merge failure: multiple non-default manifests
# が出て、出来上がった exe にマニフェストが 2 つ入る。
# spec を切るオプションは無いので、-B で先に見つかる位置に空の
# default-manifest.o を置いて差し替える (%s は startfile prefix を順に探し、
# 最初に見つかったものを使う)。実測でマニフェストは 1 つだけになる。
NOMANIFESTDIR := $(OBJDIR)/nomanifest
NOMANIFEST    := $(NOMANIFESTDIR)/default-manifest.o
LDFLAGS       += -B$(NOMANIFESTDIR)/

.PHONY: all debug clean run uitest

all: $(TARGET)

debug: $(TARGET)

# UAC を出さずに画面だけ確かめるためのビルド。
# 中身は同一で、マニフェストだけ asInvoker。変更操作は権限不足で失敗する。
uitest: $(UITARGET)

$(UITARGET): $(OBJS) $(UIRES) $(NOMANIFEST) | $(BINDIR)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(UIRES) $(LDLIBS)

# 中身が空のオブジェクト。MinGW の default-manifest.o を押しのけるためだけに
# 存在する。リソースを持たないので .rsrc の衝突が起きない。
$(NOMANIFEST): | $(OBJDIR)
	@mkdir -p $(NOMANIFESTDIR)
	@echo 'static int dnm_no_default_manifest;' > $(NOMANIFESTDIR)/empty.c
	$(CC) -c -o $@ $(NOMANIFESTDIR)/empty.c

$(UIRES): $(SRCDIR)/app.rc $(SRCDIR)/app-uitest.manifest $(SRCDIR)/resource.h | $(OBJDIR)
	$(WINDRES) -I$(SRCDIR) -DDNM_MANIFEST=\"app-uitest.manifest\" -O coff -o $@ $<

$(TARGET): $(OBJS) $(RES) $(NOMANIFEST) | $(BINDIR)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(RES) $(LDLIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/dnm.h $(SRCDIR)/resource.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# .rc は app.manifest を取り込むので、そちらの更新でも作り直す
$(RES): $(SRCDIR)/app.rc $(SRCDIR)/app.manifest $(SRCDIR)/resource.h | $(OBJDIR)
	$(WINDRES) -I$(SRCDIR) -O coff -o $@ $<

$(BINDIR):
	@mkdir -p $(BINDIR)

$(OBJDIR):
	@mkdir -p $(OBJDIR)

# mingw32-make はレシピを cmd ではなく sh で走らせるので、ここは sh の
# 書き方でよい。以前 "2>nul" と書いていたら、リダイレクトではなく nul と
# いう名前の空ファイルがプロジェクト直下にできてしまった。
# mkdir -p も rm -rf も対象が有る/無いで失敗しないため、そもそも要らない。
clean:
	@rm -rf build

run: $(TARGET)
	$(TARGET)
