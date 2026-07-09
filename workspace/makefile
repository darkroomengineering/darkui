# workspace (all)

###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
$(error please specify PLATFORM, eg. PLATFORM=trimui make)
endif

###########################################################

.PHONY: all

all:
	cd ./$(PLATFORM)/libmsettings && make
	cd ./$(PLATFORM) && make early # eg. other libs
	cd ./$(PLATFORM)/keymon && make
	cd ./all/minui/ && make
	cd ./all/minarch/ && make
	cd ./all/clock/ && make
	cd ./all/minput/ && make
	cd ./all/syncsettings/ && make
	cd ./all/say/ && make
	if [ "$(PLATFORM)" = "rg35xxplus" ]; then cd ./all/wifi/ && make; fi
	cd ./$(PLATFORM)/cores && make
	cd ./$(PLATFORM) && make

clean:
	cd ./$(PLATFORM)/libmsettings && make clean
	cd ./$(PLATFORM)/keymon && make clean
	cd ./all/minui/ && make clean
	cd ./all/minarch/ && make clean
	cd ./all/clock/ && make clean
	cd ./all/minput/ && make clean
	cd ./all/syncsettings/ && make clean
	cd ./all/say/ && make clean
	if [ "$(PLATFORM)" = "rg35xxplus" ]; then cd ./all/wifi/ && make clean; fi
	cd ./$(PLATFORM) && make clean