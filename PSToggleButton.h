//
//  PSToggleButton.h
//  Gusto
//
//  Created by Taybin on 3/17/07.
//  Copyright 2007 Penguin Sounds. All rights reserved.
//

#import <Cocoa/Cocoa.h>


@interface PSToggleButton : NSButton {
	SEL shiftAction;
}

- (void)setShiftAction:(SEL)aSelector;

@end
