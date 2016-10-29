CXX = g++ 
NETLIBS= 

all: daytime-server daytime-client http-server myhttpd git hello.so

daytime-client : daytime-client.o
	$(CXX) -o $@ $@.o $(NETLIBS)

daytime-server : daytime-server.o
	$(CXX) -o $@ $@.o $(NETLIBS)

http-server : http-server.o
	$(CXX) -o $@ $@.o $(NETLIBS)

myhttpd : myhttpd.o
	$(CXX) -o $@ $@.o $(NETLIBS) -ldl -lpthread

hello.so : hello.cc
	gcc -shared -o hello.so -fPIC hello.cc 

%.o: %.cc
	@echo 'Building $@ from $<'
	$(CXX) -o $@ -c -I. $<

git: 
	git add Makefile *.cc *.h  *.c >> .local.git.out	
	git commit -a -m "Commit HTTP Server" >> .local.git.out

clean:
	rm -f *.o use-dlopen ./http-root-dir/cgi-bin/hello.so
	rm -f *.o daytime-server daytime-client http-server myhttpd


