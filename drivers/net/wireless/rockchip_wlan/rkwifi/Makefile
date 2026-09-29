# SPDX-License-Identifier: GPL-2.0
#rkwifi packed Makefile
# (gwl)

obj-$(CONFIG_AP6XXX) += bcmdhd/

.PHONY: clean

clean:
	find . -name '*.o*' -exec rm -f {} \; 
obj-$(CONFIG_RTL8822CS)	+= rtl8822cs/
