//
//  PSEditorWindowController.h
//  Gusto
//
//  Created by taybin on 3/12/07.
//  Copyright 2007 Penguin Sounds. All rights reserved.
//

#import <Cocoa/Cocoa.h>

#import "PSControlsView.h"

@interface PSEditorWindowController : NSWindowController {
	IBOutlet PSControlsView *controlsView;
	
	ARDOUR::Session *sess;
	NSMutableArray *routeArray;
}

- (id)initWithSession:(ARDOUR::Session*)session;

- (IBAction)play:(id)sender;
- (IBAction)record:(id)sender;
- (IBAction)stop:(id)sender;

- (void)connectToSession;
- (void)initialRouteListDisplay;
- (void)routesAdded:(ARDOUR::Session::RouteList&)rl;

@end
