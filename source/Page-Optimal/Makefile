CXX ?= g++
CXXFLAGS ?= -std=c++17 -O3 -g
SOURCES := $(shell find src -name '*.cpp' -type f)
OBJECTS := $(patsubst src/%.cpp,build/%.o,$(SOURCES))
INCLUDES := $(addprefix -I,$(shell find src -type d))
.PHONY: all
all: MQSim
MQSim: $(OBJECTS)
	$(CXX) $(OBJECTS) -o $@
build/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@
-include $(OBJECTS:.o=.d)
