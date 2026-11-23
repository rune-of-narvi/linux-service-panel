CXX ?= g++
PKGS = gtk4 libadwaita-1
CPPFLAGS += -Iinclude $(shell pkg-config --cflags $(PKGS))
CXXFLAGS += -std=c++23 -Wall -Wextra -Wpedantic
gtk_libs = $(shell pkg-config --libs $(PKGS))
test_libs = $(shell pkg-config --libs glib-2.0 gtest_main)

objects = src/main.o src/backend.o src/systemd.o src/unit_row.o
test_objects = tests/unit_row_test.o src/unit_row.o
test_binary = tests/unit_row_test

service-panel: $(objects)
	$(CXX) $(LDFLAGS) $^ $(gtk_libs) $(LDLIBS) -o $@

src/main.o src/backend.o: include/backend.h
src/main.o src/backend.o src/systemd.o: include/systemd.h
$(objects): include/glib_ptr.h
$(objects) tests/unit_row_test.o: include/unit_row.h
tests/unit_row_test.o: CPPFLAGS += $(shell pkg-config --cflags gtest_main)

$(test_binary): $(test_objects)
	$(CXX) $(LDFLAGS) $^ $(test_libs) $(LDLIBS) -o $@

.PHONY: test clean
test: $(test_binary)
	./$(test_binary)

clean:
	rm -f service-panel $(objects) tests/unit_row_test.o $(test_binary)
