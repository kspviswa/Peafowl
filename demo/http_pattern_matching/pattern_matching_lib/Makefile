#GNU makefile
CXX = g++
CXXFLAGS = -O -g -Wall -W -ansi 

OBJECTS = buffer.o signatures.o timer.o trie.o

.PHONY: all clean doxygen realclean

clean:
	-rm -f *.o *~

cleanall:
	make clean
	-rm -fr libpatternmatching.a

%o : %.cc
	$(CXX) $(INCLUDES) $(CXXFLAGS) -c $< -o $@

lib: $(OBJECTS)
	ar rs libpatternmatching.a *.o
