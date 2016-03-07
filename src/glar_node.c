/*  =========================================================================
    glar_node - Glar150 service

    Copyright (c) the Contributors as noted in the AUTHORS file.       
    This file is part of the Glar150 Project.                          
                                                                       
    This Source Code Form is subject to the terms of the Mozilla Public
    License, v. 2.0. If a copy of the MPL was not distributed with this
    file, You can obtain one at http://mozilla.org/MPL/2.0/.           
    =========================================================================
*/

/*
@header
    glar_node - Glar150 service
@discuss
@end
*/

#include "glar_classes.h"
#include "glar_node_fsm.h"      //  Generated state machine engine

//  Structure of our class

struct _glar_node_t {
    bool verbose;               //  Trace activity yes/no
    fsm_t *fsm;                 //  Our finite state machine
    zyre_t *zyre;               //  Zyre node instance
    zactor_t *panel;            //  LED control panel
    zactor_t *console;          //  Command line input
    zpoller_t *poller;          //  Socket poller
    zmsg_t *msg;                //  Last message we received
    zyre_event_t *event;        //  Last zyre_event received
};

static void
s_console_actor (zsock_t *pipe, void *args)
{
    zsock_signal (pipe, 0);             //  Tell caller we're ready
    while (!zsys_interrupted) {
        char command [256];
        if (fgets (command, 256, stdin)) {
            //  Discard final newline
            command [strlen (command) - 1] = 0;
            zstr_send (pipe, command);
        }
    }
}


//  --------------------------------------------------------------------------
//  Create a new glar node

glar_node_t *
glar_node_new (const char *iface, bool console)
{
    glar_node_t *self = (glar_node_t *) zmalloc (sizeof (glar_node_t));
    assert (self);
    self->fsm = fsm_new (self);
    self->zyre = zyre_new (NULL);
    self->panel = zactor_new (glar_panel_actor, NULL);
    self->poller = zpoller_new (zyre_socket (self->zyre), NULL);
    if (console) {
        self->console = zactor_new (s_console_actor, NULL);
        zpoller_add (self->poller, self->console);
    }
    zyre_set_interface (self->zyre, iface);
    zsys_info ("using interface=%s", iface);
    return self;
}


//  --------------------------------------------------------------------------
//  Destroy the glar node

void
glar_node_destroy (glar_node_t **self_p)
{
    assert (self_p);
    if (*self_p) {
        glar_node_t *self = *self_p;
        fsm_destroy (&self->fsm);
        zyre_destroy (&self->zyre);
        zactor_destroy (&self->panel);
        zactor_destroy (&self->console);
        zpoller_destroy (&self->poller);
        zmsg_destroy (&self->msg);
        zyre_event_destroy (&self->event);
        free (self);
        *self_p = NULL;
    }
}


//  --------------------------------------------------------------------------
//  Set verbose on/off

void
glar_node_set_verbose (glar_node_t *self, bool verbose)
{
    assert (self);
    self->verbose = verbose;
    if (verbose) {
        zyre_set_verbose (self->zyre);
        fsm_set_animate (self->fsm, true);
    }
}


//  --------------------------------------------------------------------------
//  Execute state machine until finished

void
glar_node_execute (glar_node_t *self)
{
    assert (self);
    fsm_set_next_event (self->fsm, self->console? console_event: robot_event);
    fsm_execute (self->fsm);
}


//  ---------------------------------------------------------------------------
//  join_network_as_robot
//

static void
join_network_as_robot (glar_node_t *self)
{
    zyre_set_header (self->zyre, "ROLE", "%s", "robot");
    zyre_start (self->zyre);
    zyre_join (self->zyre, "GLAR");
    //  Show rotating sequence until peer joins
    zstr_send (self->panel, "100,010,001,");
    zstr_send (self->panel, "100.010.001.*");
}


//  ---------------------------------------------------------------------------
//  join_network_as_console
//

static void
join_network_as_console (glar_node_t *self)
{
    zyre_set_header (self->zyre, "ROLE", "%s", "console");
    zyre_start (self->zyre);
    zyre_join (self->zyre, "GLAR");
}


//  ---------------------------------------------------------------------------
//  wait_for_activity
//

static void
wait_for_activity (glar_node_t *self)
{
    zsock_t *which = (zsock_t *) zpoller_wait (self->poller, -1);
    if (which == zyre_socket (self->zyre)) {
        zyre_event_destroy (&self->event);
        self->event = zyre_event_new (self->zyre);
        zmsg_destroy (&self->msg);
        self->msg = zyre_event_get_msg (self->event);

        if (streq (zyre_event_type (self->event), "JOIN"))
            fsm_set_next_event (self->fsm, join_event);
        else
        if (streq (zyre_event_type (self->event), "LEAVE"))
            fsm_set_next_event (self->fsm, leave_event);
        else
        if (streq (zyre_event_type (self->event), "WHISPER"))
            fsm_set_next_event (self->fsm, whisper_event);
        else
        if (streq (zyre_event_type (self->event), "SHOUT"))
            fsm_set_next_event (self->fsm, shout_event);
        else
            fsm_set_next_event (self->fsm, other_event);
    }
    else
    if (self->console
    && (which == (void *) self->console)) {
        zmsg_destroy (&self->msg);
        self->msg = zmsg_recv (self->console);
        fsm_set_next_event (self->fsm, console_command_event);
    }
}


//  ---------------------------------------------------------------------------
//  shout_command_to_robots
//

static void
shout_command_to_robots (glar_node_t *self)
{
    zyre_shout (self->zyre, "GLAR", &self->msg);
}


//  ---------------------------------------------------------------------------
//  execute_the_command
//

static void
execute_the_command (glar_node_t *self)
{
    char *command = zmsg_popstr (self->msg);
    zsys_info ("Run command '%s'", command);
    if (system (command) == 0) {
        zsys_info ("System '%s' OK", command);
        //  Flash LED 1 once slowly
        zstr_send (self->panel, "010;000;");
        zyre_whispers (self->zyre, zyre_event_peer_uuid (self->event), "%s", "OK");
    }
    else {
        zsys_info ("System '%s' FAIL", command);
        //  Flash LED 2 once slowly
        zstr_send (self->panel, "001;000;");
        zyre_whispers (self->zyre, zyre_event_peer_uuid (self->event), "%s", "FAILED");
    }
    free (command);
}


//  ---------------------------------------------------------------------------
//  print_command_results
//

static void
print_command_results (glar_node_t *self)
{
    char *results = zmsg_popstr (self->msg);
    printf ("%s: %s\n", zyre_event_peer_name (self->event), results);
    free (results);
}


//  ---------------------------------------------------------------------------
//  signal_peer_joined
//

static void
signal_peer_joined (glar_node_t *self)
{
    if (self->console)
        zsys_info ("JOINED peer=%s", zyre_event_peer_name (self->event));
    else
        //  Flash LED 1 three times rapidly
        zstr_send (self->panel, "010,000,010,000,010,000,");
}


//  ---------------------------------------------------------------------------
//  signal_peer_left
//

static void
signal_peer_left (glar_node_t *self)
{
    if (self->console)
        zsys_info ("LEFT peer=%s", zyre_event_peer_name (self->event));
    else
        //  Flash LED 2 three times rapidly
        zstr_send (self->panel, "001,000,001,000,001,000,");
}



//  ---------------------------------------------------------------------------
//  show_at_rest_sequence
//

static void
show_at_rest_sequence (glar_node_t *self)
{
    //  At rest sequence, LED 0 slow blinking
    if (!self->console)
        zstr_send (self->panel, "100::000::*");
}


//  ---------------------------------------------------------------------------
//  leave_network
//

static void
leave_network (glar_node_t *self)
{
    zyre_leave (self->zyre, "GLAR");
    zyre_stop (self->zyre);
}


//  --------------------------------------------------------------------------
//  Self test of this class

void
glar_node_test (bool verbose)
{
    printf (" * glar_node: ");

    //  @selftest
    glar_node_t *node = glar_node_new ("wlan0", false);
    if (verbose)
        glar_node_set_verbose (node, verbose);
    assert (node);
    glar_node_execute (node);
    glar_node_destroy (&node);
    //  @end
    printf ("OK\n");
}