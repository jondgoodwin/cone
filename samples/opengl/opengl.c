// Create a simple window and set background color

#include <stdio.h>

// OpenGL / glew Headers
#define GL3_PROTOTYPES 1
#include <GL/glew.h>

// SDL2 Header
#include "SDL.h"

void swapWindow();


int running;

void stopWorld() {
    running = 0;
}

void loadWorld() {
    glClearColor(0.0, 0.0, 0.0, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);
    swapWindow();
}

void handleKey(SDL_Keycode key) {
    switch (key)
    {
    case SDLK_ESCAPE:
        stopWorld();
        break;
    case SDLK_r:
        // Cover with red and update
        glClearColor(1.0, 0.0, 0.0, 1.0);
        glClear(GL_COLOR_BUFFER_BIT);
        swapWindow();
        break;
    case SDLK_g:
        // Cover with green and update
        glClearColor(0.0, 1.0, 0.0, 1.0);
        glClear(GL_COLOR_BUFFER_BIT);
        swapWindow();
        break;
    case SDLK_b:
        // Cover with blue and update
        glClearColor(0.0, 0.0, 1.0, 1.0);
        glClear(GL_COLOR_BUFFER_BIT);
        swapWindow();
        break;
    default:
        break;
    }
}

void doEvent();
void mainWorld() {
    loadWorld();

    running = 1;
    while (running)
    {
        doEvent();
    }
}