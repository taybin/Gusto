//
//  PSAppController.h
//  Gusto
//
//  Created by Taybin Rutkin on 2/22/07.
//  Copyright 2007 Penguin Sounds. All rights reserved.
//

#import <Cocoa/Cocoa.h>

#import "PSAddTrack.h"
#import "PSPreferencesController.h"

namespace ARDOUR {
	class AudioEngine;
}

@interface PSAppController : NSObject {
	PSPreferencesController *preferencesController;
	
	ARDOUR::AudioEngine *engine;
	
	PSAddTrack *addTrackController;
	
	NSTimer *seconds;
	
	IBOutlet NSMenuItem *cpuItem;
}

+ (id)sharedManager;

- (ARDOUR::AudioEngine*)engine;
- (PSAddTrack *)addTrackController;
- (void)threadFix:(id)arg;

- (void)everySecond:(NSTimer*)theTimer;
- (void)updateCPU;

- (IBAction)newSession:(id)sender;
- (IBAction)saveSession:(id)sender;
- (IBAction)addTrackBus:(id)sender;
- (IBAction)preferences:(id)sender;

// delegates
- (BOOL)applicationShouldOpenUntitledFile:(NSApplication *)sender;

@end
