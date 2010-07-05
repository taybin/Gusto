//
//  PSRouteUI.h
//  Gusto
//
//  Created by Taybin on 3/16/07.
//  Copyright 2007 Penguin Sounds. All rights reserved.
//

#import <Cocoa/Cocoa.h>

#include <boost/shared_ptr.hpp>

#import "PSToggleButton.h"

namespace ARDOUR {
	class Route;
	class Session;
}

// Abstract class
@interface PSRouteUI : NSObject {
	IBOutlet PSToggleButton *recordBtn;
	IBOutlet PSToggleButton *muteBtn;
	IBOutlet PSToggleButton *soloBtn;
	
	NSString *name;
	
	boost::shared_ptr<ARDOUR::Route> route;
	ARDOUR::Session* session;
}

- (id)initWithRoute:(boost::shared_ptr<ARDOUR::Route>)r withSession:(ARDOUR::Session*)s;

- (BOOL)isAudioTrack;

- (IBAction)mutePressed:(id)sender;
- (IBAction)muteShiftClicked:(id)sender;
- (void)updateMute;

- (IBAction)soloPressed:(id)sender;
- (IBAction)soloShiftClicked:(id)sender;
- (void)updateSolo;

- (IBAction)recordPressed:(id)sender;
- (IBAction)recordShiftClicked:(id)sender;
- (void)updateRecord;

- (NSString*)name;
- (void)setName:(NSString *)n;
- (void)updateName;

@end

void muteDisplayGlue (void*, id);
void soloDisplayGlue (void*, id);
void recordDisplayGlue (void*, id);
void nameDisplayGlue (void*, id);
