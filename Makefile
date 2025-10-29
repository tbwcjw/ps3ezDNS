#---------------------------------------------------------------------------------
# Clear the implicit built in rules
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(PSL1GHT)),)
$(error "Please set PSL1GHT in your environment. export PSL1GHT=<path>")
endif

#---------------------------------------------------------------------------------
#  TITLE, APPID, CONTENTID, ICON0 SFOXML before ppu_rules.
#---------------------------------------------------------------------------------
TITLE		    := ezDNS
APPID		    := PS3EZD001
CONTENTID	    := UP0001-$(APPID)_00-0000000000000000
ICON0           := $(CURDIR)/pkgfiles/ICON0.PNG
#ICON1          := $(CURDIR)/ICON1.PAM
#PIC1           := $(CURDIR)/PIC1.PNG
SFOXML          := $(CURDIR)/pkgfiles/sfo.xml

SCETOOL_FLAGS	?=	--self-app-version=0001000000000000  --sce-type=SELF --compress-data=TRUE --self-add-shdrs=TRUE --skip-sections=FALSE --key-revision=1 \
					--self-auth-id=1010000001000003 --self-vendor-id=01000002 --self-fw-version=0004002000000000

include $(PSL1GHT)/ppu_rules

# aditional scetool flags (--self-ctrl-flags, --self-cap-flags...)
# --self-ctrl-flags Root Flags + debug_mode_capability flags (vsh)
# --self-cap-flags lv2_kernel
SCETOOL_FLAGS	+=	--self-ctrl-flags 4000000000000000000000000000000000000000000000000000000000000002
SCETOOL_FLAGS	+=	--self-cap-flags 00000000000000000000000000000000000000000000007B0000000100000000

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# INCLUDES is a list of directories containing extra header files
#---------------------------------------------------------------------------------
TARGET		:=	ezDNS
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
SHADERS		:=	shaders
INCLUDES	:=	include ../include lib/include
PKGFILES	:=	$(CURDIR)/pkgfiles

#---------------------------------------------------------------------------------
# any extra libraries we wish to link with the project
#---------------------------------------------------------------------------------
LIBS		:=	-lsysfs -lfreetype -lio -lnet -lpthread \
				-lz -ltiny3d -lgcm_sys -lsysutil -lrt \
				-lsysmodule -lm

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
#optional: 
CFLAGS		=	-O2 -Wall -mcpu=cell $(MACHDEP) $(INCLUDE) -DPS3_GEKKO $(DFLAGS) -DVERSION=\"$(VERSION)\" -DDEBUG -DDEBUG_ADDR=\"10.0.0.152\" -DDEBUG_PORT=\"18194\" -DPS3LOADX
CXXFLAGS	=	$(CFLAGS)

LDFLAGS		=	$(MACHDEP) -Wl,-Map,$(notdir $@).map

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:=

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------
#We grab APP_VER from sfo.xml to name our zip archive, and display in app/web dashboard.
export VERSION := $(strip $(shell xmllint --xpath 'normalize-space(/sfo/value[@name="APP_VER"]/text())' $(SFOXML)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir)) \
					$(foreach dir,$(SHADERS),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

export BUILDDIR	:=	$(CURDIR)/$(BUILD)

#---------------------------------------------------------------------------------
# automatically build a list of object files for our project
#---------------------------------------------------------------------------------
CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
BINFILES	:= $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.bin)))
VCGFILES	:=	$(foreach dir,$(SHADERS),$(notdir $(wildcard $(dir)/*.vcg)))
FCGFILES	:=	$(foreach dir,$(SHADERS),$(notdir $(wildcard $(dir)/*.fcg)))

VPOFILES	:=	$(VCGFILES:.vcg=.vpo)
FPOFILES	:=	$(FCGFILES:.fcg=.fpo)

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES	:=	$(addsuffix .o,$(BINFILES)) \
					$(addsuffix .o,$(VPOFILES)) \
					$(addsuffix .o,$(FPOFILES)) \
					$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) \
					$(sFILES:.s=.o) $(SFILES:.S=.o)

#---------------------------------------------------------------------------------
# build a list of include paths
#---------------------------------------------------------------------------------
export INCLUDE	:=	$(foreach dir,$(INCLUDES), -I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					$(LIBPSL1GHT_INC) \
					-I$(CURDIR)/$(BUILD) -I$(PORTLIBS)/include \
					-I$(PS3DEV)/portlibs/ppu/include/freetype2

#---------------------------------------------------------------------------------
# build a list of library paths
#---------------------------------------------------------------------------------
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
					$(LIBPSL1GHT_LIB) -L$(PORTLIBS)/lib

export OUTPUT	:=	$(CURDIR)/$(TARGET)
.PHONY: $(BUILD) clean


#---------------------------------------------------------------------------------
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self  EBOOT.BIN

#---------------------------------------------------------------------------------
run:
	ps3load $(OUTPUT).self

#---------------------------------------------------------------------------------
pkg:	$(BUILD) $(OUTPUT).pkg

#---------------------------------------------------------------------------------
release: clean pkg
	zip -r "$(OUTPUT)_$(VERSION).zip" $(TARGET).pkg README.md LICENSE
#---------------------------------------------------------------------------------

npdrm: $(BUILD)
	@$(SELF_NPDRM) $(SCETOOL_FLAGS) --np-content-id=$(CONTENTID) --encrypt $(BUILDDIR)/$(basename $(notdir $(OUTPUT))).elf $(BUILDDIR)/../EBOOT.BIN

#---------------------------------------------------------------------------------

else

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf:	$(OFILES)

#---------------------------------------------------------------------------------
# This rule links in binary data with the .bin extension
#---------------------------------------------------------------------------------
%.bin.o	:	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

#---------------------------------------------------------------------------------
%.vpo.o	:	%.vpo
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

#---------------------------------------------------------------------------------
%.fpo.o	:	%.fpo
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
