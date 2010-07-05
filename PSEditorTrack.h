//
//  PSEditorTrack.h
//  Gusto
//
//  Created by taybin on 3/8/07.
//  Copyright 2007 Penguin Sounds. All rights reserved.
//

#import <Cocoa/Cocoa.h>

#include <boost/shared_ptr.hpp>

#import "PSRouteUI.h"

namespace ARDOUR {
	class Route;
	class Session;
}

@interface PSEditorTrack : PSRouteUI {
	IBOutlet NSView *trackControl;
	IBOutlet NSView *regionCanvas;
}

- (NSView*)trackControl;

@end
