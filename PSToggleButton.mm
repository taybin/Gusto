//
//  PSToggleButton.mm
//  Gusto
//
//  Created by Taybin on 3/17/07.
//  Copyright 2007 Penguin Sounds. All rights reserved.
//

#import "PSToggleButton.h"


@implementation PSToggleButton

- (void)setShiftAction:(SEL)aSelector
{
	shiftAction = aSelector;
}

- (void)mouseDown:(NSEvent *)theEvent
{
	[self setState:![self state]];
	if (shiftAction && ([theEvent modifierFlags] & NSShiftKeyMask)) {
		[NSApp sendAction:shiftAction to:[self target] from:self];
	} else {
		[NSApp sendAction:[self action] to:[self target] from:self];
	}
}

- (void)otherMouseDown:(NSEvent *)theEvent
{
	// ensure it's a middle button click
	if([theEvent buttonNumber] == 2) {
		[self mouseDown:theEvent];
	}
}

- (void)otherMouseUp:(NSEvent *)theEvent
{
	[self otherMouseDown:theEvent];
}

@end
