//
//  PSAppController.mm
//  Gusto
//
//  Created by Taybin Rutkin on 2/22/07.
//  Copyright 2007 Penguin Sounds. All rights reserved.
//

#undef check
#include <glibmm.h>
#include <pbd/id.h>
#include <pbd/pthread_utils.h>

#include <ardour/configuration.h>
#include <ardour/utils.h>
#include <ardour/audioengine.h>
#include <ardour/session.h>

#import "PSAppController.h"
#import "PSNewDialogController.h"

static PSAppController *sharedAppControllerManager = nil;

@implementation PSAppController

- (void)dealloc
{
	[addTrackController release];
	[seconds invalidate];
	[preferencesController release];
	
	delete engine;
	ARDOUR::cleanup ();
	
	pthread_cancel_all ();

	[super dealloc];
}

+ (id)sharedManager 
{
	return sharedAppControllerManager;
}

- (ARDOUR::AudioEngine*)engine
{
	return engine;
}

- (void)threadFix:(id)arg
{
	return;
}

- (void)awakeFromNib
{
	// this turns on multiThreaded mode
	[NSThread detachNewThreadSelector:@selector(threadFix:) toTarget:self withObject:nil];
	
	sharedAppControllerManager = self;

	NSString* path = [[NSBundle mainBundle] bundlePath];
	path = [path stringByAppendingString:@"/Contents/MacOS"];
	setenv("PATH", [path UTF8String], 1);
	
	Glib::thread_init();

	pthread_setcanceltype (PTHREAD_CANCEL_ASYNCHRONOUS, 0);
	
	PBD::ID::init ();
	
	try {
		ARDOUR::init (false, false);
		ARDOUR::Config->set_current_owner (ARDOUR::ConfigVariableBase::Interface);
				
		while (engine == 0) {
			try {
				engine = new ARDOUR::AudioEngine ("Gusto");
			} catch (std::exception& err) {   // can't seem to catch ARDOUR::AudioEngine::NoBackendAvailable
				NSAlert *alert = [[NSAlert alloc] init];
				[alert addButtonWithTitle:@"Quit"];
				[alert addButtonWithTitle:@"Retry"];
				[alert setMessageText:@"Could not connect to Jack"];
				[alert setInformativeText:[NSString stringWithUTF8String:err.what()]];
				[alert setAlertStyle:NSCriticalAlertStyle];
							
				if ([alert runModal] == NSAlertFirstButtonReturn) {
					[NSApp terminate:self];
				}
				
				[alert release];
			}
		}
		
		try {
			engine->start();
		} catch (...) {
			NSLog(@"Unable to start the engine");
			[NSApp terminate:self];
		}
		
	} catch (failed_constructor& err) {
		NSAlert *alert = [[NSAlert alloc] init];
		[alert addButtonWithTitle:@"Quit"];
		[alert setMessageText:@"Could not initialize backend"];
		[alert setAlertStyle:NSCriticalAlertStyle];
		
		[alert runModal];
		[alert release];
		
		[NSApp terminate:self];
	}
		
	seconds = [NSTimer scheduledTimerWithTimeInterval:1 
											   target:self
											 selector:@selector(everySecond:)
											 userInfo:nil
											  repeats:YES];
}

- (PSAddTrack *)addTrackController
{
	if (addTrackController == 0) {
		addTrackController = [[PSAddTrack alloc] init];
	}
	
	return addTrackController;
}

- (void)everySecond:(NSTimer*)theTimer
{
	[self updateCPU];
}

- (void)updateCPU
{
	char buf[32];
	snprintf (buf, sizeof (buf), "DSP: %.1f%%", engine->get_cpu_load());
	[cpuItem setTitle:[NSString stringWithCString:buf]];
	NSLog(@"%@", cpuItem);
}

- (IBAction)newSession:(id)sender
{
	using namespace ARDOUR;
	
	PSNewDialogController *ndc = [[PSNewDialogController alloc] init];
	NSURL *url = [ndc newFile]; 

	AutoConnectOption iconnect;
	AutoConnectOption oconnect;
	
	if ([ndc autoPhysIn]) {
		iconnect = AutoConnectPhysical;
	} else {
		iconnect = AutoConnectOption(0);
	}
	
	if ([ndc autoOuts]) {
		int choice = [ndc outputChoice];
		if (choice == 0) {
			oconnect = AutoConnectMaster;
		} else if (choice == 1) {
			oconnect = AutoConnectPhysical;
		}
	} else {
		oconnect = AutoConnectOption(0);
	}
	
	AudioEngine *eng = [[PSAppController sharedManager] engine];
	
	NSLog(@"%s",[[url path] fileSystemRepresentation]);
	NSLog([[url path] lastPathComponent]);
	
	ARDOUR::Session *sess;
	try {
		if ([ndc useTemplate]) {
			NSString *templatePath = [[NSBundle mainBundle] bundlePath];
			templatePath = [templatePath stringByAppendingString:@"/Contents/Resources/Templates/"];
			templatePath = [templatePath stringByAppendingString:[ndc chosenTemplate]];
			templatePath = [templatePath stringByAppendingString:@".template"];
			std::string cTemplatePath([templatePath UTF8String]);

			sess = new Session (*eng,
								string([[url path] fileSystemRepresentation]), string([[[url path] lastPathComponent] UTF8String]),
								cTemplatePath);
		} else {
			sess = new Session (*eng, 
								[[url path] fileSystemRepresentation], [[[url path] lastPathComponent] UTF8String], 
								iconnect, oconnect,
								[ndc nMonitors], [ndc nMasters],
								[ndc nPhysicalInputs], [ndc nPhysicalOutputs],
								eng->frame_rate() * 60 * 5);
		}
		sess->save_state("");
	} catch (...) {
		NSLog(@"could not create session");
	}
			
	delete sess;
	
	[[NSDocumentController sharedDocumentController] openDocumentWithContentsOfURL:url display:YES error:nil];
	
	[ndc release];
}

- (IBAction)saveSession:(id)sender
{
	[[[NSDocumentController sharedDocumentController] currentDocument] saveSession];
}

- (IBAction)addTrackBus:(id)sender
{
	[[[NSDocumentController sharedDocumentController] currentDocument] addTrack:self];
}

- (IBAction)preferences:(id)sender
{
	if (!preferencesController) {
		preferencesController = [[PSPreferencesController alloc] init];
	}
	
	[preferencesController showWindow:self];
}

- (BOOL)applicationShouldOpenUntitledFile:(NSApplication *)sender
{
	return NO;
}

@end
