/*
    Copyright 2017 Nanoleaf Ltd.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */



#include "AuroraPlugin.h"
#include "LayoutProcessingUtils.h"
#include "ColorUtils.h"
#include "DataManager.h"
#include "PluginFeatures.h"
#include "Logger.h"
#include "SoundUtils.h"
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

	void initPlugin();
	void getPluginFrame(Frame_t* frames, int* nFrames, int* sleepTime);
	void pluginCleanup();

#ifdef __cplusplus
}
#endif

#define MAX_SOURCES 15
#define BASE_COLOUR_R 0
#define BASE_COLOUR_G 0
#define BASE_COLOUR_B 0
#define TRANSITION_TIME 1
#define ENERGY_THRESHOLD 50000
#define MAX_DIFFUSION_AGE 15.0
#define MIN_SIMULTANEOUS_COLOURS 2
#define N_FFT_BINS 32
#define ADJACENT_PANEL_DISTANCE 86.599995
#define BEAT_COUNT 8
#define FRACTION_COLOUR_TO_KEEP 0.05

static RGB_t* paletteColours = NULL; // saved pointer to colour palette
static int nColours = 0;             // number of colours in the palette
static LayoutData *layoutData;       // saved pointer to panel layout information

// Store info associated with each light source like current
// position, velocity, and colour. The information is stored in a list called sources.
typedef struct {
    float x;
    float y;
    float vx;
    float vy;
    float diffusion_age; // starts from zero and increments for each frame
    int R;
    int G;
    int B;
    float intensity;
    float speed;
    uint16_t energy;
} source_t;
static source_t sources[MAX_SOURCES]; // array of sources
static int nSources = 0;


/**
 * @description: Initialize the plugin. Called once, when the plugin is loaded.
 * This function can be used to enable rhythm or advanced features,
 * e.g., to enable energy feature, simply call enableEnergy()
 * It can also be used to load the LayoutData and the colorPalette from the DataManager.
 * Any allocation, if done here, should be deallocated in the plugin cleanup function
 *
 */
void initPlugin(){
    getColorPalette(&paletteColours, &nColours); // grab the palette colours
    PRINTLOG("The palette has %d colours:\n", nColours);

    for (int i = 0; i < nColours; i++) {
        PRINTLOG("   %d %d %d\n", paletteColours[i].R, paletteColours[i].G, paletteColours[i].B);
    }

    layoutData = getLayoutData(); // grab the layout data

    PRINTLOG("The layout has %d panels:\n", layoutData->nPanels);

    for (int i = 0; i < layoutData->nPanels; i++) {
        PRINTLOG("   Id: %d   X, Y: %lf, %lf\n", layoutData->panels[i].panelId,
                 layoutData->panels[i].shape->getCentroid().x, layoutData->panels[i].shape->getCentroid().y);
    }

    // enable features
    enableEnergy();
    enableFft(N_FFT_BINS);
    enableBeatFeatures();
}

/** Removes a light source from the list of light sources */
void removeSource(int idx) {
    memmove(sources + idx, sources + idx + 1, sizeof(source_t) * (nSources - idx - 1));
    nSources--;
}

/** Compute catesian distance between two points */
float distance(float x1, float y1, float x2, float y2) {
    float dx = x2 - x1;
    float dy = y2 - y1;
    return sqrt(dx * dx + dy * dy);
}

/**
 * @description: Get a colour by interpolating in a linear way amongs the set of colours in the palette
 * @param colour The colour we want between 0 and nColours - 1. We interpolate in the palette to come up
 *               with a final colour somewhere in the palettes spectrum.
 */
void getRGB(float colour, int *returnR, int *returnG, int *returnB)
{
    float R;
    float G;
    float B;

    if(nColours == 0) {
        *returnR = 128; // in the case of no palette, use half white as default
        *returnG = 128;
        *returnB = 128;
    }
    else if(nColours == 1) {
        *returnR = paletteColours[0].R;
        *returnG = paletteColours[0].G;
        *returnB = paletteColours[0].B;
    }
    else {
        int idx = (int)colour;
        float fraction = colour - (float)idx;

        if(colour <= 0) {
            R = paletteColours[0].R;
            G = paletteColours[0].G;
            B = paletteColours[0].B;
        }
        else if(idx < nColours - 1) {
            float R1 = paletteColours[idx].R;
            float G1 = paletteColours[idx].G;
            float B1 = paletteColours[idx].B;
            float R2 = paletteColours[idx + 1].R;
            float G2 = paletteColours[idx + 1].G;
            float B2 = paletteColours[idx + 1].B;
            R = (1.0 - fraction) * R1 + fraction * R2;
            G = (1.0 - fraction) * G1 + fraction * G2;
            B = (1.0 - fraction) * B1 + fraction * B2;
        }
        else {
            R = paletteColours[nColours - 1].R;
            G = paletteColours[nColours - 1].G;
            B = paletteColours[nColours - 1].B;
        }
        *returnR = (int)R;
        *returnG = (int)G;
        *returnB = (int)B;
    }
}


/**
 * @description: Adds a light source to the list of light sources. The light source will have a particular colour
 * and intensity and will move at a particular speed.
 */
void addSource(float colour, float intensity, float speed, uint16_t energy)
{
    // pick a panel light source is centred on
    static int r = 0;
    static int count = 0;

    float x = layoutData->panels[r].shape->getCentroid().x;
    float y = layoutData->panels[r].shape->getCentroid().y;
    count++;
    if (count >= BEAT_COUNT) {
        r = (int)(drand48() * layoutData->nPanels - 1);
        count = 0;
    }

    // decide in the colour of this light source and factor in the intensity to arrive at an RGB value
    int R;
    int G;
    int B;
    getRGB(colour, &R, &G, &B);
    R *= intensity;
    G *= intensity;
    B *= intensity;

    // if too many light sources then remove the oldest one
    if (nSources >= MAX_SOURCES) {
        removeSource(0);
    }

    int i;

    for (i = 0; i < nSources; i++) {
        if (sources[i].x == x && sources[i].y == y) {
            sources[i].diffusion_age = 0.0;
            sources[i].intensity = intensity;
            sources[i].speed = speed;
            return;
        }
    }

    // sources ordered by increasing intensity??
    // find the last light source with less intensity than the current one
    for (i = nSources; i > 0; i--) {
        if (intensity >= sources[i - 1].intensity) {
            break;
        }
    }

    memmove(sources + i + 1, sources + i, sizeof(source_t) * (nSources - i));

    // save the information in the list
    sources[i].x = x;
    sources[i].y = y;
    sources[i].diffusion_age = 0.0;
    sources[i].R = R;
    sources[i].G = G;
    sources[i].B = B;
    sources[i].intensity = intensity;
    sources[i].speed = speed;
    sources[i].energy = energy;
		if (energy >= ENERGY_THRESHOLD) {
			count = BEAT_COUNT;
		}
    nSources++;

}

/**
 * @description: This function will render the colour of the given single panel given
 * the positions of all the lights in the light source list.
 */
void renderPanel(Panel *panel, int *returnR, int *returnG, int *returnB)
{
    float R = BASE_COLOUR_R;
    float G = BASE_COLOUR_G;
    float B = BASE_COLOUR_B;


    int i;
    for (i = 0; i < nSources; i++) {
        float diffusion_age = sources[i].diffusion_age;
        float d = distance(panel->shape->getCentroid().x, panel->shape->getCentroid().y, sources[i].x, sources[i].y);

        float factor = 1.0;
				if (sources[i].energy >= ENERGY_THRESHOLD) {
					float d2 = d * 0.015;
					d2 = d2 - (diffusion_age * 0.2);
					if (d2 < 0.0) {
						d2 = 0.0;
					}
					factor = 1.0 / (d2 * 2.0 + 1.0);
					if (factor > 1.0) {
						factor = 1.0;
					}
					if (factor < 0.0) {
						factor = 0.0;
					}
				} else {
					factor = 1.0 / (d * 0.008 + 1.0);
				}

        // diffusion factor
        if (diffusion_age >= MAX_DIFFUSION_AGE) {
            factor = 0.0;
        } else {
            factor = factor * (1.0 - diffusion_age / MAX_DIFFUSION_AGE);
        }

        if(factor < FRACTION_COLOUR_TO_KEEP) {
            factor = FRACTION_COLOUR_TO_KEEP;
        }

        R = R * (1.0 - factor) + sources[i].R * factor;
        G = G * (1.0 - factor) + sources[i].G * factor;
        B = B * (1.0 - factor) + sources[i].B * factor;

        // change hue based on distance
        HSV_t hsv;
        RGB_t rgb;
        RGBtoHSV((RGB_t){(int)R, (int)G, (int)B}, &hsv);
        int d_factor = d * 0.10;
        hsv.H = (hsv.H + d_factor) % 360;
        hsv.V = (hsv.V + 10) % 360;
        HSVtoRGB(hsv, &rgb);

        R = rgb.R;
        G = rgb.G;
        B = rgb.B;

    }

    *returnR = (int)R;
    *returnG = (int)G;
    *returnB = (int)B;

    if (*returnR > 255) {
        *returnR = 255;
    }
    if (*returnG > 255) {
        *returnG = 255;
    }
    if (*returnB > 255) {
        *returnB = 255;
    }
}


/**
 * Increase the diffusion age number of each light source. In the rendering routine, this has the
 * effect of increasing the radious of the light source and decreasing its brightness. If any
 * light source gets too old, it will be removed from the list.
 */
void diffuseSources(void)
{
    int i;

		for (i = 0; i < nSources; i++) {
			sources[i].diffusion_age += sources[i].speed;
		}

    if(nSources > MIN_SIMULTANEOUS_COLOURS) {
        for(i = 0; i < nSources; i++) {
            if(sources[i].diffusion_age > MAX_DIFFUSION_AGE) {
                removeSource(i);
                i--;
            }
        }
    }
}

/**
 * @description: this the 'main' function that gives a frame to the Aurora to display onto the panels
 * To obtain updated values of enabled features, simply call get<feature_name>, e.g.,
 * getEnergy(), getIsBeat().
 *
 * If the plugin is a sound visualization plugin, the sleepTime variable will be NULL and is not required to be
 * filled in
 * This function, if is an effects plugin, can specify the interval it is to be called at through the sleepTime variable
 * if its a sound visualization plugin, this function is called at an interval of 50ms or more.
 *
 * @param frames: a pre-allocated buffer of the Frame_t structure to fill up with RGB values to show on panels.
 * Maximum size of this buffer is equal to the number of panels
 * @param nFrames: fill with the number of frames in frames
 * @param sleepTime: specify interval after which this function is called again, NULL if sound visualization plugin
 */
void getPluginFrame(Frame_t* frames, int* nFrames, int* sleepTime){
    int R;
    int G;
    int B;
    int i;
    static int maxBinIndexSum = 0;
    static int n = 0;

    // figure out what frequency is strongest
    int maxBin = 0;
    int maxBinIndex = 0;
    uint8_t *fftBins = getFftBins();
    for (i = 0; i < N_FFT_BINS; i++) {
        if (fftBins[i] > maxBin) {
            maxBin = fftBins[i];
            maxBinIndex = i;
        }
    }
		//visualizeFft(fftBins, N_FFT_BINS);
    maxBinIndexSum += maxBinIndex;
    n++;

    uint16_t energy = getEnergy();

    // When a beat is detected, we will choose a colour and display it
    // The colour depends on the strongest frequencies that have been measured since the last beat
    // The speed depends on the current tempo
    // The first palette colour is reserved for onsets and the rest are used for beats
    if (getIsBeat()) {
        maxBinIndex = maxBinIndexSum / n;
        maxBinIndexSum = 0;
        n = 0;
        int colour = maxBinIndex * nColours / (N_FFT_BINS / 4);
        float speed = (float)getTempo() / 50;
        if (speed < 0.2) {
            speed = 0.2;
        }
        float intensity = 1.0;

        addSource(colour, intensity, 1.5, energy);
    } else if (getIsOnset()) {
        addSource(drand48() * (nColours - 1), 0.7, 0.8, energy);
    }

    for (i = 0; i < layoutData->nPanels; i++) {
        renderPanel(&layoutData->panels[i], &R, &G, &B);
        frames[i].panelId = layoutData->panels[i].panelId;
        frames[i].r = R;
        frames[i].g = G;
        frames[i].b = B;
        frames[i].transTime = TRANSITION_TIME;
    }

    // diffuse all the light sources so they are ready for the next frame
    diffuseSources();

    // this algorithm renders every panel at every frame
    *nFrames = layoutData->nPanels;
}

/**
 * @description: called once when the plugin is being closed.
 * Do all deallocation for memory allocated in initplugin here
 */
void pluginCleanup(){
	//do deallocation here
}
