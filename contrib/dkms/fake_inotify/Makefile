obj-m += fake_inotify.o

all:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

install:
	cp fake_inotify.ko /lib/modules/$(shell uname -r)/kernel/drivers/misc/

uninstall:
	rm /lib/modules/$(shell uname -r)/kernel/drivers/misc/fake_inotify.ko
