SRCS=$(wildcard $(SRCDIR)/*.c)
OBJS=$(SRCS:.c=.OBJ)
EXE=THREAD

#SHELL="./fakeshell.sh"
#SHELL="echo"
DOSBOX_APP=./DOSBox.app
#DOSBOX=open -a $(DOSBOX_APP)
DOSBOX=$(DOSBOX_APP)/Contents/MacOS/DOSBox
DOSROOT=dosroot
DOS_BUILD_SCRIPT=$(DOSROOT)/BUILD.BAT
BATCH_MAKER=dos.sh

SRCDIR=$(DOSROOT)/src
CC=TCC
INCLUDES=/I\\TC\\INCLUDE
LINKER=TLINK
LIBS=/L\\TC\\LIB\\

export DOS_BUILD_SCRIPT

: exec
.PHONY: clean

all: build

exec: build
	./dos.sh $(EXE)

build: $(DOS_BUILD_SCRIPT) $(EXE)
	# Launch DOSBox
	$(DOSBOX) $(DOS_BUILD_SCRIPT)

$(BATCH_MAKER):
	echo "#!/bin/bash" > $(BATCH_MAKER)
	echo 'echo "$$@" >> $$DOS_BUILD_SCRIPT' >> $(BATCH_MAKER)
	chmod +x $(BATCH_MAKER)

$(EXE): batch-prepare $(BATCH_MAKER)
	./dos.sh $(CC) /v $(INCLUDES) $(LIBS) /e$@ $(notdir $(SRCS))

$(DOS_BUILD_SCRIPT): batch-reset batch-prepare $(SRCS)

batch-reset:
	rm -f $(DOS_BUILD_SCRIPT)

batch-prepare: batch-reset $(BATCH_MAKER)
	./dos.sh C:
	./dos.sh cd src
	./dos.sh set PATH=C:\\TC\;C:\\TC\\BIN\;Z:\\\;
	./dos.sh set CLASSPATH=C:\\TC\\LIB\;
	./dos.sh set LIB=C:\\TC\\LIB\;
	./dos.sh set INCLUDE=C:\\TC\\INCLUDE\;

batch-clean: $(BATCH_MAKER)
	./dos.sh del *.EXE
	./dos.sh del *.OBJ
	./dos.sh del *.MAP

clean: batch-reset
	rm -f $(SRCS:.c=.MAP) $(OBJS) $(SRCDIR)/$(EXE).EXE $(BATCH_MAKER)
