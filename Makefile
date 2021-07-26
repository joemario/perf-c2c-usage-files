CC = gcc
LDLIBS = -lnuma -lpthread
binary = false_sharing.exe
source = false_sharing_example.c
.PHONY : clean

$(binary) : $(source)
	$(CC) $(LDLIBS) -o $@ $<
clean :
	-rm $(binary) $(objects)
