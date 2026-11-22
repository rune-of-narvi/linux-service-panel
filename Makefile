CXX ?= g++
PKGS = gtk4 libadwaita-1
CPPFLAGS += -Iinclude $(shell pkg-config --cflags $(PKGS))
CXXFLAGS += -std=c++23 -Wall -Wextra -Wpedantic
LDLIBS += $(shell pkg-config --libs $(PKGS))

objects = src/main.o src/systemd.o

gnome-service-manager: $(objects)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(objects): include/systemd.h

.PHONY: clean
clean:
	rm -f gnome-service-manager $(objects)
