CXX ?= g++
PKGS = gtk4 libadwaita-1
CPPFLAGS += $(shell pkg-config --cflags $(PKGS))
CXXFLAGS += -std=c++23 -Wall -Wextra -Wpedantic
LDLIBS += $(shell pkg-config --libs $(PKGS))

gnome-service-manager: main.o systemd.o
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

main.o systemd.o: systemd.hpp

.PHONY: clean
clean:
	rm -f gnome-service-manager *.o
