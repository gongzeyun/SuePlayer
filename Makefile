#Author: gongzeyun
#makefile of SuePlayer

SuePlayer:player.c
	gcc -o SuePlayer player.c -lavutil -lavformat -lavcodec -lz -lavutil -lpthread -lm -lswscale -lavfilter -lswresample -lSDL2 -lSDL2_ttf
clean:
	rm SuePlayer
