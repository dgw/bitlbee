  /********************************************************************\
  * BitlBee -- An IRC to other IM-networks gateway                     *
  *                                                                    *
  * Copyright 2002-2010 Wilmer van der Gaast and others                *
  \********************************************************************/

/* The IRC-based UI - Representing (virtual) channels.                  */

/*
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License with
  the Debian GNU/Linux distribution in /usr/share/common-licenses/GPL;
  if not, write to the Free Software Foundation, Inc., 59 Temple Place,
  Suite 330, Boston, MA  02111-1307  USA
*/

#include "bitlbee.h"

irc_channel_t *irc_channel_new( irc_t *irc, const char *name )
{
	irc_channel_t *ic;
	
	if( strchr( CTYPES, name[0] ) == NULL || !nick_ok( name + 1 ) )
		return NULL;
	
	ic = g_new0( irc_channel_t, 1 );
	ic->irc = irc;
	ic->name = g_strdup( name );
	strcpy( ic->mode, CMODE );
	
	irc_channel_add_user( ic, irc->root );
	
	irc->channels = g_slist_prepend( irc->channels, ic );
	
	return ic;
}

int irc_channel_add_user( irc_channel_t *ic, irc_user_t *iu )
{
	if( g_slist_find( ic->users, iu ) != NULL )
		return 0;
	
	ic->users = g_slist_insert_sorted( ic->users, iu, irc_user_cmp );
	
	if( iu == ic->irc->user || ic->flags & IRC_CHANNEL_JOINED )
	{
		ic->flags |= IRC_CHANNEL_JOINED;
		irc_send_join( ic, iu );
	}
	
	return 1;
}

int irc_channel_del_user( irc_channel_t *ic, irc_user_t *iu )
{
	if( g_slist_find( ic->users, iu ) == NULL )
		return 0;
	
	ic->users = g_slist_remove( ic->users, iu );
	
	if( ic->flags & IRC_CHANNEL_JOINED )
		irc_send_part( ic, iu, "" );
	
	if( iu == ic->irc->user )
		ic->flags &= ~IRC_CHANNEL_JOINED;
	
	return 1;
}

int irc_channel_set_topic( irc_channel_t *ic, const char *topic )
{
	g_free( ic->topic );
	ic->topic = g_strdup( topic );
	
	if( ic->flags & IRC_CHANNEL_JOINED )
		irc_send_topic( ic );
	
	return 1;
}
