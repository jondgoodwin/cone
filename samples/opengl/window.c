// Create a simple window and set background color

#include <stdio.h>

// OpenGL / glew Headers
#define GL3_PROTOTYPES 1
#include <GL/glew.h>

// SDL2 Header
#include "SDL.h"

SDL_Window *mainWindow;
SDL_GLContext mainContext;

void CheckSDLError() {
    const char *error = SDL_GetError();
    if (*error) {
        printf("SDL Error : %s\n", error);
        SDL_ClearError();
    }
}

int init()
{
    // Initialize SDL's Video subsystem
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
    {
        printf("Failed to initialize SDL\n");
        return 0;
    }

    // Create our window centered at 512x512 resolution
    mainWindow = SDL_CreateWindow("OpenGL/SDL window", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        512, 512, SDL_WINDOW_OPENGL);

    // Check that everything worked out okay
    if (!mainWindow)
    {
        printf("Unable to create window\n");
        CheckSDLError();
        return 0;
    }

    // Create our opengl context and attach it to our window
    mainContext = SDL_GL_CreateContext(mainWindow);

    // Set our OpenGL version.
    // SDL_GL_CONTEXT_CORE gives us only the newer version, deprecated functions are disabled
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    // 3.2 is part of the modern versions of OpenGL, but most video cards whould be able to run it
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);

    // Turn on double buffering with a 24bit Z buffer.
    // You may need to change this to 16 or 32 for your system
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    // This makes our buffer swap synchronized with the monitor's vertical refresh
    SDL_GL_SetSwapInterval(1);

    // Init GLEW
    glewExperimental = GL_TRUE;
    glewInit();

    return 1;
}

void cleanup()
{
    // Delete our OpengL context
    SDL_GL_DeleteContext(mainContext);

    // Destroy our window
    SDL_DestroyWindow(mainWindow);

    // Shutdown SDL 2
    SDL_Quit();
}

void swapWindow() {
    SDL_GL_SwapWindow(mainWindow);
}

void handleKey(SDL_Keycode key);
void stopWorld();
void doEvent() {
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_QUIT)
            stopWorld();
        if (event.type == SDL_KEYDOWN)
            handleKey(event.key.keysym.sym);
    }
}

void mainWorld();
int main(int argc, char *argv[])
{
    if (!init())
        return -1;
    mainWorld();
    cleanup();
    return 0;
}