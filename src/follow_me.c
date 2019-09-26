#include "defines.h"
#include "../include/event_object_movement.h"
#include "../include/field_door.h"
#include "../include/field_effect.h"
#include "../include/field_player_avatar.h"
#include "../include/field_screen_effect.h"
#include "../include/field_weather.h"
#include "../include/fieldmap.h"
#include "../include/fldeff_misc.h"
#include "../include/item.h"
#include "../include/overworld.h"
#include "../include/script.h"
#include "../include/constants/event_object_movement_constants.h"
#include "../include/constants/event_objects.h"

#include "../include/new/item.h"
#include "../include/new/follow_me.h"
#include "../include/new/frontier.h"
#include "../include/new/util.h"
#include "../include/new/overworld.h"

/**
 * \file follow_me.c
 * \brief Functions for NPCs following the player.
 */
 
//This file's functions:
static u8 GetFollowerMapObjId(void);
static u8 GetFollowerSprite(void);
static void Task_ReallowPlayerMovement(u8 taskId);
static u8 DetermineFollowerDirection(struct EventObject* player, struct EventObject* follower);
static void PlayerLogCoordinates(struct EventObject* player);
static u8 DetermineFollowerState(struct EventObject* follower, u8 state, u8 direction);
static bool8 IsStateMovement(u8 state);
static u8 ReturnFollowerDelayedState(u8 direction);
static void SetSurfJump(struct EventObject* npc);
static void Task_BindSurfBlobToFollower(u8 taskId);
static void SetUpSurfBlobFieldEffect(struct EventObject* npc);
static void SetSurfDismount(struct EventObject* npc);
static void Task_FinishSurfDismount(u8 taskId);
static void Task_FollowerOutOfDoor(u8 taskId);
static void Task_FollowerHandleIndoorStairs(u8 taskId);
static void Task_FollowerHandleEscalator(u8 taskId);
static void Task_FollowerHandleEscalatorFinish(u8 taskId);
static void CalculateFollowerEscalatorTrajectoryUp(struct Task *task);
static void CalculateFollowerEscalatorTrajectoryDown(struct Task *task);
static void SetFollowerSprite(u8 spriteIndex);
static void TurnNPCIntoFollower(u8 localId, u8 followerFlags);

#undef gFieldEffectArguments
#define gFieldEffectArguments ((u32*) 0x20386E0)

#define MOVEMENT_INVALID 0xFE

enum
{
	FOLLOWER_SPRITE_INDEX_NORMAL,
	FOLLOWER_SPRITE_INDEX_BIKE,
	FOLLOWER_SPRITE_INDEX_SURF,
};

struct FollowerSprites
{
	u16 normalId;
	u16 bikeId;
	u16 surfId;
};

static const struct FollowerSprites gFollowerAlternateSprites[] =
{
	//FORMAT:
	//{WALKING/RUNNING SPRITE ID, BIKING SPRITE ID, SURFING SPRITE ID},
	//EXAMPLE: {MALE PLAYER WALKING, MALE PLAYER BIKING, MALE PLAYER SURFING},
	{0, 1, 2},

	#ifdef UNBOUND
	{EVENT_OBJ_GFX_JOGGER, EVENT_OBJ_GFX_CYCLIST_M, EVENT_OBJ_GFX_SWIMMER_M}, //For debugging
	{EVENT_OBJ_GFX_COLLECTOR, EVENT_OBJ_GFX_CYCLIST_M, EVENT_OBJ_GFX_SWIMMER_M}, //For debugging
	#endif
};

//General Utility
u8 GetFollowerObjectId(void)
{
	if (!gFollowerState.inProgress)
		return EVENT_OBJECTS_COUNT;

	return gFollowerState.objId;
}

u8 GetFollowerLocalId(void)
{
	if (!gFollowerState.inProgress)
		return 0;

	return gEventObjects[gFollowerState.objId].localId;
}

const u8* GetFollowerScriptPointer(void)
{
	if (!gFollowerState.inProgress)
		return NULL;

	return gFollowerState.script;
}

void HideFollower(void)
{
	if (!gFollowerState.inProgress)
		return;
	
	if (gFollowerState.createSurfBlob == 2 || gFollowerState.createSurfBlob == 3)
	{
		BindFieldEffectToSprite(gEventObjects[GetFollowerMapObjId()].fieldEffectSpriteId, 2);
		DestroySprite(&gSprites[gEventObjects[GetFollowerMapObjId()].fieldEffectSpriteId]);
		gEventObjects[GetFollowerMapObjId()].fieldEffectSpriteId = 0; //Unbind
	}
	
	gEventObjects[GetFollowerMapObjId()].invisible = TRUE;
}

void IsFollowerStoppingRockClimb(void)
{
	gSpecialVar_LastResult = FALSE;
	if (!gFollowerState.inProgress)
		return;

	gSpecialVar_LastResult = (gFollowerState.flags & FOLLOWER_FLAG_CAN_ROCK_CLIMB) == 0;
}

void FollowMe_SetIndicatorToComeOutDoor(void)
{
	if (gFollowerState.inProgress)
		gFollowerState.comeOutDoorStairs = 1;
}

static u8 GetFollowerMapObjId(void)
{
	return gFollowerState.objId;
}

static u8 GetFollowerSprite(void)
{
	switch (gFollowerState.currentSprite) {
		case FOLLOWER_SPRITE_INDEX_BIKE:
			for (u32 i = 0; i < ARRAY_COUNT(gFollowerAlternateSprites); ++i)
			{
				if (gFollowerAlternateSprites[i].normalId == gFollowerState.gfxId)
					return gFollowerAlternateSprites[i].bikeId;
			}
			break;
		case FOLLOWER_SPRITE_INDEX_SURF:
			for (u32 i = 0; i < ARRAY_COUNT(gFollowerAlternateSprites); ++i)
			{
				if (gFollowerAlternateSprites[i].normalId == gFollowerState.gfxId)
					return gFollowerAlternateSprites[i].surfId;
			}
			break;
	}

	return gFollowerState.gfxId;
}

//Actual Follow Me
void FollowMe(struct EventObject* npc, u8 state, bool8 ignoreScriptActive)
{
	struct EventObject* player = &gEventObjects[gPlayerAvatar->eventObjectId];

	if (player != npc) //Only when the player moves
		return;

	if (!gFollowerState.inProgress)
		return;

	if (ScriptContext2_IsEnabled() && !ignoreScriptActive)
		return; //Don't follow during a script

	struct EventObject* follower = &gEventObjects[GetFollowerMapObjId()];

	//Check if state would cause hidden follower to reappear
	if (IsStateMovement(state) && gFollowerState.warpEnd)
	{
		gFollowerState.warpEnd = 0;

		if (gFollowerState.comeOutDoorStairs == 1)
		{
			gPlayerAvatar->preventStep = TRUE;
			u8 taskId = CreateTask(Task_FollowerOutOfDoor, 1);
			gTasks[taskId].data[0] = 0;
			gTasks[taskId].data[2] = follower->currentCoords.x;
			gTasks[taskId].data[3] = follower->currentCoords.y;
			goto RESET;
		}
		else if (gFollowerState.comeOutDoorStairs == 2)
			gFollowerState.comeOutDoorStairs = FALSE;
	
		follower->invisible = FALSE;
		MoveEventObjectToMapCoords(follower, player->currentCoords.x, player->currentCoords.y);
		EventObjectTurn(follower, player->facingDirection); //The follower should be facing the same direction as the player when it comes out of hiding

		if (gFollowerState.createSurfBlob == 2) //Recreate surf blob
		{
			SetUpSurfBlobFieldEffect(follower);
			follower->fieldEffectSpriteId = FieldEffectStart(FLDEFF_SURF_BLOB);
			BindFieldEffectToSprite(follower->fieldEffectSpriteId, 1);
		}
	}

	u8 dir = DetermineFollowerDirection(player, follower);

	if (dir == DIR_NONE)
		goto RESET;

	u8 newState = DetermineFollowerState(follower, state, dir);

	if (newState == MOVEMENT_INVALID)
		goto RESET;

	if (gFollowerState.createSurfBlob == 1) //Get on Surf Blob
	{
		gFollowerState.createSurfBlob = 2;
		gPlayerAvatar->preventStep = TRUE; //Wait for finish
		SetSurfJump(&gEventObjects[GetFollowerMapObjId()]);
		goto RESET;
	}
	else if (gFollowerState.createSurfBlob == 3) //Get off Surf Blob
	{
		gFollowerState.createSurfBlob = 0; 
		gPlayerAvatar->preventStep = TRUE; //Wait for finish
		SetSurfDismount(&gEventObjects[GetFollowerMapObjId()]);
		goto RESET;
	}

	EventObjectClearHeldMovementIfActive(follower);
	EventObjectSetHeldMovement(follower, newState);
	PlayerLogCoordinates(player);
	
	*((u8*) 0x2023D6C) = newState;
	
	switch (newState) {
		case MOVEMENT_ACTION_JUMP_2_DOWN ... MOVEMENT_ACTION_JUMP_2_RIGHT:
		case 0x84 ... 0x87:
		case MOVEMENT_ACTION_WALK_NORMAL_LEFT_DOWN_FACE_DOWN ... MOVEMENT_ACTION_WALK_NORMAL_RIGHT_UP_FACE_RIGHT:
		case MOVEMENT_ACTION_RUN_LEFT_DOWN_FACE_DOWN ... MOVEMENT_ACTION_RUN_RIGHT_UP_FACE_RIGHT:
		case MOVEMENT_ACTION_WALK_FAST_LEFT_DOWN_FACE_DOWN ... MOVEMENT_ACTION_WALK_FAST_RIGHT_UP_FACE_RIGHT:
			CreateTask(Task_ReallowPlayerMovement, 1); //Synchronize movements on stairs and ledges
			gPlayerAvatar->preventStep = TRUE;
	}

RESET:
	EventObjectClearHeldMovementIfFinished(follower);
}

static void Task_ReallowPlayerMovement(u8 taskId)
{
	bool8 animStatus = EventObjectClearHeldMovementIfFinished(&gEventObjects[GetFollowerMapObjId()]);
	if (animStatus == 0)
	{
		if (TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_DASH)
		&& EventObjectClearHeldMovementIfFinished(&gEventObjects[gPlayerAvatar->eventObjectId]))
			SetPlayerAvatarTransitionFlags(PLAYER_AVATAR_FLAG_ON_FOOT); //Temporarily stop running
		return;
	}

	gPlayerAvatar->preventStep = FALSE;
	DestroyTask(taskId);
}

static u8 DetermineFollowerDirection(struct EventObject* player, struct EventObject* follower)
{
	//Move the follower towards the player
	s8 delta_x = follower->currentCoords.x - player->currentCoords.x;
	s8 delta_y = follower->currentCoords.y - player->currentCoords.y;

	if (delta_x < 0)
		return DIR_EAST;
	else if (delta_x > 0)
		return DIR_WEST;

	if (delta_y < 0)
		return DIR_SOUTH;
	else if (delta_y > 0)
		return DIR_NORTH;

	return DIR_NONE;
}

static void PlayerLogCoordinates(struct EventObject* player)
{
	gFollowerState.log.x = player->currentCoords.x;
	gFollowerState.log.y = player->currentCoords.y;
}

#define RETURN_STATE(state, dir) return newState == MOVEMENT_INVALID ? state + (dir - 1) : ReturnFollowerDelayedState(dir - 1);

static u8 DetermineFollowerState(struct EventObject* follower, u8 state, u8 direction)
{
	u8 newState = MOVEMENT_INVALID;

	if (IsStateMovement(state) && gFollowerState.delayedState)
		newState = gFollowerState.delayedState + direction;

	*((u8*) 0x2023D6B) = state;

	//Clear ice tile stuff
	follower->disableAnim = FALSE; //follower->field1 &= 0xFB;

	switch (state) {
		case MOVEMENT_ACTION_WALK_SLOWEST_DOWN ... MOVEMENT_ACTION_WALK_SLOWEST_RIGHT:
			// Slow walk
			RETURN_STATE(MOVEMENT_ACTION_WALK_SLOWEST_DOWN, direction);

		case MOVEMENT_ACTION_WALK_SLOW_DOWN ... MOVEMENT_ACTION_WALK_SLOW_RIGHT:
			// Slow slow
			RETURN_STATE(MOVEMENT_ACTION_WALK_SLOW_DOWN, direction);

		case MOVEMENT_ACTION_WALK_NORMAL_DOWN ... MOVEMENT_ACTION_WALK_NORMAL_RIGHT:
			// Normal walk
			RETURN_STATE(MOVEMENT_ACTION_WALK_NORMAL_DOWN, direction);

		case MOVEMENT_ACTION_JUMP_2_DOWN ... MOVEMENT_ACTION_JUMP_2_RIGHT:
			// Ledge jump
			if (((newState - direction) >= MOVEMENT_ACTION_JUMP_2_DOWN && (newState - direction) <= MOVEMENT_ACTION_JUMP_2_RIGHT)
			||  ((newState - direction) >= 0x84 && (newState - direction) <= 0x87)) //Previously jumped
			{
				newState = MOVEMENT_INVALID;
				RETURN_STATE(MOVEMENT_ACTION_JUMP_2_DOWN, direction); //Jump right away
			}

			gFollowerState.delayedState = MOVEMENT_ACTION_JUMP_2_DOWN;
			RETURN_STATE(MOVEMENT_ACTION_WALK_NORMAL_DOWN, direction);

		case MOVEMENT_ACTION_WALK_FAST_DOWN ... MOVEMENT_ACTION_WALK_FAST_RIGHT:
			 //Handle ice tile (some walking animation)
			 //Set a bit to freeze the follower's animation
			 //FIXME: Use a hook (at 08063E28) to set this bit
			follower->disableAnim = TRUE;
			RETURN_STATE(MOVEMENT_ACTION_WALK_FAST_DOWN, direction);
			
		case MOVEMENT_ACTION_SLIDE_SLOW_DOWN ... MOVEMENT_ACTION_SLIDE_SLOW_RIGHT:
			//Slow slide or Bike Speed
			RETURN_STATE(MOVEMENT_ACTION_SLIDE_SLOW_DOWN, direction);
			
		case MOVEMENT_ACTION_SLIDE_DOWN ... MOVEMENT_ACTION_SLIDE_RIGHT:
			//Slide
			RETURN_STATE(MOVEMENT_ACTION_SLIDE_DOWN, direction);
			
		case MOVEMENT_ACTION_SLIDE_FAST_DOWN ... MOVEMENT_ACTION_SLIDE_FAST_DOWN:
			//Slide Fast
			RETURN_STATE(MOVEMENT_ACTION_SLIDE_FAST_DOWN, direction);
		
		case MOVEMENT_ACTION_SLIDE_RIGHT_FOOT_DOWN ... MOVEMENT_ACTION_SLIDE_RIGHT_FOOT_RIGHT:
			//Running frames
			if (gFollowerState.flags & FOLLOWER_FLAG_HAS_RUNNING_FRAMES)
				RETURN_STATE(MOVEMENT_ACTION_SLIDE_RIGHT_FOOT_DOWN, direction);
			
			RETURN_STATE(MOVEMENT_ACTION_WALK_FAST_DOWN, direction);


		case MOVEMENT_ACTION_SLIDE_LEFT_FOOT_DOWN ... MOVEMENT_ACTION_SLIDE_LEFT_FOOT_RIGHT:
			//Stairs (slow walking)
			//Running sideways on stairs does not use the slow
			//frames, so split this into two.
			if (direction < 2)
			{
				RETURN_STATE(MOVEMENT_ACTION_SLIDE_LEFT_FOOT_DOWN, direction);
			}
			else
			{
				RETURN_STATE(MOVEMENT_ACTION_SLIDE_RIGHT_FOOT_DOWN, direction);
			}

		case MOVEMENT_ACTION_JUMP_SPECIAL_DOWN ... MOVEMENT_ACTION_JUMP_SPECIAL_RIGHT:
			gFollowerState.delayedState = MOVEMENT_ACTION_JUMP_SPECIAL_DOWN;
			RETURN_STATE(MOVEMENT_ACTION_WALK_NORMAL_DOWN, direction);
			
		case MOVEMENT_ACTION_JUMP_DOWN ... MOVEMENT_ACTION_JUMP_RIGHT:
			gFollowerState.delayedState = MOVEMENT_ACTION_JUMP_DOWN;
			RETURN_STATE(MOVEMENT_ACTION_WALK_NORMAL_DOWN, direction);
			
		case 0x84 ... 0x87: ;
			//Ledge run
			if (((newState - direction) >= MOVEMENT_ACTION_JUMP_2_DOWN && (newState - direction) <= MOVEMENT_ACTION_JUMP_2_RIGHT)
			||  ((newState - direction) >= 0x84 && (newState - direction) <= 0x87)) //Previously jumped
			{
				newState = MOVEMENT_INVALID;
				if (gFollowerState.flags & FOLLOWER_FLAG_HAS_RUNNING_FRAMES)
				{
					RETURN_STATE(0x84, direction); //Jump right away
				}
				else
				{
					RETURN_STATE(MOVEMENT_ACTION_JUMP_2_DOWN, direction); //Jump right away
				}
			}

			if (gFollowerState.flags & FOLLOWER_FLAG_HAS_RUNNING_FRAMES)
				gFollowerState.delayedState = 0x84;
			else
				gFollowerState.delayedState = MOVEMENT_ACTION_JUMP_2_DOWN;

			if (gFollowerState.flags & FOLLOWER_FLAG_HAS_RUNNING_FRAMES)
				RETURN_STATE(MOVEMENT_ACTION_SLIDE_RIGHT_FOOT_DOWN, direction);
			
			RETURN_STATE(MOVEMENT_ACTION_WALK_FAST_DOWN, direction);

		case MOVEMENT_ACTION_SPIN_PAD_DOWN ... MOVEMENT_ACTION_SPIN_PAD_RIGHT:
			//Warp pad spinning movement
			if (newState == state + direction)
			{
				newState = MOVEMENT_INVALID;
				RETURN_STATE(MOVEMENT_ACTION_SPIN_PAD_DOWN, direction);
			}

			gFollowerState.delayedState = MOVEMENT_ACTION_SPIN_PAD_DOWN;
			RETURN_STATE(MOVEMENT_ACTION_WALK_FAST_DOWN, direction);
			
		case MOVEMENT_ACTION_WALK_SLOWEST_UP_BACKWARDS ... MOVEMENT_ACTION_WALK_SLOWEST_LEFT_BACKWARDS:
			RETURN_STATE(MOVEMENT_ACTION_WALK_SLOWEST_UP_BACKWARDS, direction);

		case MOVEMENT_ACTION_WALK_SLOW_UP_BACKWARDS ... MOVEMENT_ACTION_WALK_SLOW_LEFT_BACKWARDS:
			RETURN_STATE(MOVEMENT_ACTION_WALK_SLOW_UP_BACKWARDS, direction);
	
		case MOVEMENT_ACTION_WALK_NORMAL_UP_BACKWARDS ... MOVEMENT_ACTION_WALK_NORMAL_LEFT_BACKWARDS:
			RETURN_STATE(MOVEMENT_ACTION_WALK_NORMAL_UP_BACKWARDS, direction);

		case MOVEMENT_ACTION_WALK_FAST_UP_BACKWARDS ... MOVEMENT_ACTION_WALK_FAST_LEFT_BACKWARDS:
			RETURN_STATE(MOVEMENT_ACTION_WALK_FAST_UP_BACKWARDS, direction);
	
		case MOVEMENT_ACTION_JUMP_2_UP_BACKWARDS ... MOVEMENT_ACTION_JUMP_2_LEFT_BACKWARDS:
			RETURN_STATE(MOVEMENT_ACTION_JUMP_2_UP_BACKWARDS, direction);
			
		case MOVEMENT_ACTION_JUMP_UP_BACKWARDS ... MOVEMENT_ACTION_JUMP_LEFT_BACKWARDS:
			RETURN_STATE(MOVEMENT_ACTION_JUMP_UP_BACKWARDS, direction);
			
		case MOVEMENT_ACTION_WALK_NORMAL_LEFT_DOWN_FACE_DOWN ... MOVEMENT_ACTION_WALK_NORMAL_RIGHT_UP_FACE_RIGHT:
			//Walk up/down side stairs
			if (newState == state + direction) //Still walking up/down stairs same direction
			{
				newState = MOVEMENT_INVALID;
				RETURN_STATE(state, 1); //Each is its own movement
			}
			else if (newState - direction >= MOVEMENT_ACTION_WALK_NORMAL_LEFT_DOWN_FACE_DOWN
			&&  newState - direction <= MOVEMENT_ACTION_WALK_FAST_RIGHT_UP_FACE_RIGHT) //Change direction on stairs
			{
				gFollowerState.delayedState = state;

				//Get new state but at different speed
				newState -= direction;
				u8 simpleState = MOVEMENT_ACTION_WALK_NORMAL_LEFT_DOWN_FACE_DOWN;
				switch (newState) {
					case MOVEMENT_ACTION_WALK_NORMAL_LEFT_DOWN_FACE_DOWN ... MOVEMENT_ACTION_WALK_NORMAL_RIGHT_UP_FACE_RIGHT:
						state = (newState - MOVEMENT_ACTION_WALK_NORMAL_LEFT_DOWN_FACE_DOWN) + simpleState;
						break;
					case MOVEMENT_ACTION_RUN_LEFT_DOWN_FACE_DOWN ... MOVEMENT_ACTION_RUN_RIGHT_UP_FACE_RIGHT:
						state = (newState - MOVEMENT_ACTION_RUN_LEFT_DOWN_FACE_DOWN) + simpleState;
						break;
					case MOVEMENT_ACTION_WALK_FAST_LEFT_DOWN_FACE_DOWN ... MOVEMENT_ACTION_WALK_FAST_RIGHT_UP_FACE_RIGHT:
						state = (newState - MOVEMENT_ACTION_WALK_FAST_LEFT_DOWN_FACE_DOWN) + simpleState;
						break;
				}

				newState = MOVEMENT_INVALID;
				RETURN_STATE(state, 1); //Each is its own movement
			}

			//Beginning on stairs
			gFollowerState.delayedState = state;
			RETURN_STATE(MOVEMENT_ACTION_WALK_NORMAL_DOWN, direction); //Each is its own movement

		case MOVEMENT_ACTION_RUN_LEFT_DOWN_FACE_DOWN ... MOVEMENT_ACTION_RUN_RIGHT_UP_FACE_RIGHT: ;
			//Run up/down side stairs
			u8 delayState;
			u8 simpleState;
			u8 action;

			if (gFollowerState.flags & FOLLOWER_FLAG_HAS_RUNNING_FRAMES)
			{
				delayState = state;
				simpleState = MOVEMENT_ACTION_RUN_LEFT_DOWN_FACE_DOWN;
				action = MOVEMENT_ACTION_SLIDE_RIGHT_FOOT_DOWN;
			}
			else
			{
				delayState = (state - MOVEMENT_ACTION_RUN_LEFT_DOWN_FACE_DOWN) + MOVEMENT_ACTION_WALK_FAST_LEFT_DOWN_FACE_DOWN;
				simpleState = MOVEMENT_ACTION_WALK_FAST_LEFT_DOWN_FACE_DOWN;
				action = MOVEMENT_ACTION_WALK_FAST_DOWN;
			}

			if (newState == state + direction)
			{
				newState = MOVEMENT_INVALID;
				RETURN_STATE(delayState, 1); //Each is its own movement
			}
			else if (newState - direction >= MOVEMENT_ACTION_WALK_NORMAL_LEFT_DOWN_FACE_DOWN
			&&  newState - direction <= MOVEMENT_ACTION_WALK_FAST_RIGHT_UP_FACE_RIGHT)
			{
				newState -= direction;
				switch (newState) {
					case MOVEMENT_ACTION_WALK_NORMAL_LEFT_DOWN_FACE_DOWN ... MOVEMENT_ACTION_WALK_NORMAL_RIGHT_UP_FACE_RIGHT:
						state = (newState - MOVEMENT_ACTION_WALK_NORMAL_LEFT_DOWN_FACE_DOWN) + simpleState;
						break;
					case MOVEMENT_ACTION_RUN_LEFT_DOWN_FACE_DOWN ... MOVEMENT_ACTION_RUN_RIGHT_UP_FACE_RIGHT:
						state = (newState - MOVEMENT_ACTION_RUN_LEFT_DOWN_FACE_DOWN) + simpleState;
						break;
					case MOVEMENT_ACTION_WALK_FAST_LEFT_DOWN_FACE_DOWN ... MOVEMENT_ACTION_WALK_FAST_RIGHT_UP_FACE_RIGHT:
						state = (newState - MOVEMENT_ACTION_WALK_FAST_LEFT_DOWN_FACE_DOWN) + simpleState;
						break;
				}

				gFollowerState.delayedState = delayState;
				newState = MOVEMENT_INVALID;
				RETURN_STATE(state, 1); //Each is its own movement
			}

			gFollowerState.delayedState = delayState;
			RETURN_STATE(action, direction); //Each is its own movement

		case MOVEMENT_ACTION_WALK_FAST_LEFT_DOWN_FACE_DOWN ... MOVEMENT_ACTION_WALK_FAST_RIGHT_UP_FACE_RIGHT: ;
			//Bike up/down side stairs
			if (newState == state + direction)
			{
				newState = MOVEMENT_INVALID;
				RETURN_STATE(state, 1); //Each is its own movement
			}
			else if (newState - direction >= MOVEMENT_ACTION_WALK_NORMAL_LEFT_DOWN_FACE_DOWN
			&&  newState - direction <= MOVEMENT_ACTION_WALK_FAST_RIGHT_UP_FACE_RIGHT)
			{
				gFollowerState.delayedState = state;

				newState -= direction;
				u8 simpleState = MOVEMENT_ACTION_WALK_FAST_LEFT_DOWN_FACE_DOWN;
				switch (newState) {
					case MOVEMENT_ACTION_WALK_NORMAL_LEFT_DOWN_FACE_DOWN ... MOVEMENT_ACTION_WALK_NORMAL_RIGHT_UP_FACE_RIGHT:
						state = (newState - MOVEMENT_ACTION_WALK_NORMAL_LEFT_DOWN_FACE_DOWN) + simpleState;
						break;
					case MOVEMENT_ACTION_RUN_LEFT_DOWN_FACE_DOWN ... MOVEMENT_ACTION_RUN_RIGHT_UP_FACE_RIGHT:
						state = (newState - MOVEMENT_ACTION_RUN_LEFT_DOWN_FACE_DOWN) + simpleState;
						break;
					case MOVEMENT_ACTION_WALK_FAST_LEFT_DOWN_FACE_DOWN ... MOVEMENT_ACTION_WALK_FAST_RIGHT_UP_FACE_RIGHT:
						state = (newState - MOVEMENT_ACTION_WALK_FAST_LEFT_DOWN_FACE_DOWN) + simpleState;
						break;
				}

				newState = MOVEMENT_INVALID;
				RETURN_STATE(state, 1); //Each is its own movement
			}

			gFollowerState.delayedState = state;
			RETURN_STATE(MOVEMENT_ACTION_WALK_FAST_DOWN, direction); //Each is its own movement

		default:
			return MOVEMENT_INVALID;
	}

	return newState;
}

static bool8 IsStateMovement(u8 state)
{
	switch (state) {
		case MOVEMENT_ACTION_FACE_DOWN:
		case MOVEMENT_ACTION_FACE_UP:
		case MOVEMENT_ACTION_FACE_LEFT:
		case MOVEMENT_ACTION_FACE_RIGHT:
		case MOVEMENT_ACTION_FACE_DOWN_FAST:
		case MOVEMENT_ACTION_FACE_UP_FAST:
		case MOVEMENT_ACTION_FACE_LEFT_FAST:
		case MOVEMENT_ACTION_FACE_RIGHT_FAST:
		case MOVEMENT_ACTION_DELAY_1:
		case MOVEMENT_ACTION_DELAY_2:
		case MOVEMENT_ACTION_DELAY_4:
		case MOVEMENT_ACTION_DELAY_8:
		case MOVEMENT_ACTION_DELAY_16:
		case MOVEMENT_ACTION_FACE_PLAYER:
		case MOVEMENT_ACTION_FACE_AWAY_PLAYER:
		case MOVEMENT_ACTION_LOCK_FACING_DIRECTION:
		case MOVEMENT_ACTION_UNLOCK_FACING_DIRECTION:
		case MOVEMENT_ACTION_SET_INVISIBLE:
		case MOVEMENT_ACTION_SET_VISIBLE:
		case MOVEMENT_ACTION_EMOTE_EXCLAMATION_MARK:
		case MOVEMENT_ACTION_EMOTE_QUESTION_MARK:
		case MOVEMENT_ACTION_EMOTE_CROSS:
		case MOVEMENT_ACTION_EMOTE_DOUBLE_EXCLAMATION_MARK:
		case MOVEMENT_ACTION_EMOTE_HAPPY:
		case MOVEMENT_ACTION_WALK_IN_PLACE_NORMAL_DOWN:
		case MOVEMENT_ACTION_WALK_IN_PLACE_NORMAL_UP:
		case MOVEMENT_ACTION_WALK_IN_PLACE_NORMAL_LEFT:
		case MOVEMENT_ACTION_WALK_IN_PLACE_NORMAL_RIGHT:
		case MOVEMENT_ACTION_WALK_IN_PLACE_FAST_DOWN:
		case MOVEMENT_ACTION_WALK_IN_PLACE_FAST_UP:
		case MOVEMENT_ACTION_WALK_IN_PLACE_FAST_LEFT:
		case MOVEMENT_ACTION_WALK_IN_PLACE_FAST_RIGHT:
		case MOVEMENT_ACTION_WALK_IN_PLACE_FASTEST_DOWN:
		case MOVEMENT_ACTION_WALK_IN_PLACE_FASTEST_UP:
		case MOVEMENT_ACTION_WALK_IN_PLACE_FASTEST_LEFT:
		case MOVEMENT_ACTION_WALK_IN_PLACE_FASTEST_RIGHT:
		case MOVEMENT_ACTION_JUMP_IN_PLACE_DOWN:
		case MOVEMENT_ACTION_JUMP_IN_PLACE_UP:
		case MOVEMENT_ACTION_JUMP_IN_PLACE_LEFT:
		case MOVEMENT_ACTION_JUMP_IN_PLACE_RIGHT:
		case MOVEMENT_ACTION_JUMP_IN_PLACE_DOWN_UP:
		case MOVEMENT_ACTION_JUMP_IN_PLACE_UP_DOWN:
		case MOVEMENT_ACTION_JUMP_IN_PLACE_LEFT_RIGHT:
		case MOVEMENT_ACTION_JUMP_IN_PLACE_RIGHT_LEFT:
			return FALSE;
	}

	return TRUE;
}

static u8 ReturnFollowerDelayedState(u8 direction)
{
	u8 newState = gFollowerState.delayedState;
	gFollowerState.delayedState = 0;
	
	switch (newState) {
		case MOVEMENT_ACTION_WALK_NORMAL_LEFT_DOWN_FACE_DOWN ... MOVEMENT_ACTION_WALK_NORMAL_RIGHT_UP_FACE_RIGHT:
		case MOVEMENT_ACTION_RUN_LEFT_DOWN_FACE_DOWN ... MOVEMENT_ACTION_RUN_RIGHT_UP_FACE_RIGHT:
		case MOVEMENT_ACTION_WALK_FAST_LEFT_DOWN_FACE_DOWN ... MOVEMENT_ACTION_WALK_FAST_RIGHT_UP_FACE_RIGHT:
			return newState; //Each its own movement, so don't modify direction
	}

	return newState + direction;
}

#define LEDGE_FRAMES_MULTIPLIER 2

extern void (**stepspeeds[5])(struct Sprite*, u8);
extern const u16 stepspeed_seq_length[5];

void FollowMe_Ledges(struct EventObject* npc, struct Sprite* sprite, u16* ledgeFramesTbl)
{
	u8 speed;

	if (!gFollowerState.inProgress)
		return;

	struct EventObject* follower = &gEventObjects[GetFollowerMapObjId()];

	if (follower == npc)
		speed = gPlayerAvatar->runningState ? 3 : 1;
	else
		speed = 0;

	//Calculate the frames for the jump
	u16 frameCount = (u16) stepspeed_seq_length[speed] * LEDGE_FRAMES_MULTIPLIER;
	ledgeFramesTbl[sprite->data[4]] = frameCount;

	//Call the step shifter
	u8 currentFrame = sprite->data[6] / LEDGE_FRAMES_MULTIPLIER;
	stepspeeds[speed][currentFrame](sprite, sprite->data[3]);
}

bool8 FollowMe_IsCollisionExempt(struct EventObject* obstacle, struct EventObject* collider)
{
	if (!gFollowerState.inProgress)
		return FALSE;

	struct EventObject* follower = &gEventObjects[GetFollowerMapObjId()];
	struct EventObject* player = &gEventObjects[gPlayerAvatar->eventObjectId];

	if (obstacle == follower && collider == player)
		return TRUE;

	return FALSE;
}

void FollowMe_FollowerToWater(void)
{
	if (!gFollowerState.inProgress)
		return;

	//Prepare for making the follower do the jump and spawn the surf head
	//right in front of the follower's location.
	FollowMe(&gEventObjects[gPlayerAvatar->eventObjectId], MOVEMENT_ACTION_JUMP_DOWN, TRUE);
	gFollowerState.createSurfBlob = 1;
}

void FollowMe_BindToSurbBlobOnReloadScreen(void)
{
	if (!gFollowerState.inProgress)
		return;
		
	if (gFollowerState.createSurfBlob != 2 && gFollowerState.createSurfBlob != 3)
		return;

	struct EventObject* npc = &gEventObjects[GetFollowerMapObjId()];

	//Spawn surfhead under follower
	SetUpSurfBlobFieldEffect(npc);
	npc->fieldEffectSpriteId = FieldEffectStart(FLDEFF_SURF_BLOB);
	BindFieldEffectToSprite(npc->fieldEffectSpriteId, 1);
}

static void SetSurfJump(struct EventObject* npc)
{
	//Reset NPC movement bits
	EventObjectClearHeldMovement(npc);

	//Jump animation according to direction
	u8 direction = DetermineFollowerDirection(&gEventObjects[gPlayerAvatar->eventObjectId], npc);
	u8 jumpState = GetJumpMovementAction(direction);
	SetUpSurfBlobFieldEffect(npc);

	//Adjust surf head spawn location infront of npc
	switch (direction) {
		case DIR_SOUTH:
			gFieldEffectArguments[1]++; //effect_y
			break;

		case DIR_NORTH:
			gFieldEffectArguments[1]--;
			break;

		case DIR_WEST:
			gFieldEffectArguments[0]--; //effect_x
			break;

		default: //DIR_EAST
			gFieldEffectArguments[0]++;
	};

	//Execute, store sprite ID in fieldEffectSpriteId and bind surf blob
	npc->fieldEffectSpriteId = FieldEffectStart(FLDEFF_SURF_BLOB);
	CreateTask(Task_BindSurfBlobToFollower, 0x1);
	SetFollowerSprite(FOLLOWER_SPRITE_INDEX_SURF);
	EventObjectSetHeldMovement(npc, jumpState);
}

static void Task_BindSurfBlobToFollower(u8 taskId)
{
	struct EventObject* npc = &gEventObjects[GetFollowerMapObjId()];

	//Wait jump animation
	bool8 animStatus = EventObjectClearHeldMovementIfFinished(npc);
	if (animStatus == 0)
		return;

	//Bind objs
	BindFieldEffectToSprite(npc->fieldEffectSpriteId, 0x1);
	UnfreezeEventObjects();
	DestroyTask(taskId);
	gPlayerAvatar->preventStep = FALSE; //Player can move again
	return;
}

static void SetUpSurfBlobFieldEffect(struct EventObject* npc)
{
	//Set up gFieldEffectArguments for execution
	gFieldEffectArguments[0] = npc->currentCoords.x; 	//effect_x
	gFieldEffectArguments[1] = npc->currentCoords.y;	//effect_y
	gFieldEffectArguments[2] = gFollowerState.objId;    //objId
}

void PrepareFollowerDismountSurf(void)
{
	if (!gFollowerState.inProgress)
		return;

	FollowMe(&gEventObjects[gPlayerAvatar->eventObjectId], MOVEMENT_ACTION_WALK_NORMAL_DOWN, TRUE);
	gFollowerState.createSurfBlob = 3;
}

static void SetSurfDismount(struct EventObject* npc)
{
	EventObjectClearHeldMovement(npc);

	//Jump animation according to direction
	u8 direction = DetermineFollowerDirection(&gEventObjects[gPlayerAvatar->eventObjectId], npc);
	u8 jumpState = GetJumpMovementAction(direction);

	//Unbind and destroy Surf Blob
	u8 task = CreateTask(Task_FinishSurfDismount, 1);
	gTasks[task].data[0] = npc->fieldEffectSpriteId;
	BindFieldEffectToSprite(npc->fieldEffectSpriteId, 2);
	gEventObjects[GetFollowerMapObjId()].fieldEffectSpriteId = 0; //Unbind
	FollowMe_HandleSprite();
	EventObjectSetHeldMovement(npc, jumpState);
}

static void Task_FinishSurfDismount(u8 taskId)
{
	struct EventObject* npc = &gEventObjects[GetFollowerMapObjId()];

	//Wait animation
	bool8 animStatus = EventObjectClearHeldMovementIfFinished(npc);
	if (animStatus == 0)
	{
		if (TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_DASH)
		&& EventObjectClearHeldMovementIfFinished(&gEventObjects[gPlayerAvatar->eventObjectId]))
			SetPlayerAvatarTransitionFlags(PLAYER_AVATAR_FLAG_ON_FOOT); //Temporarily stop running
		return;
	}

	DestroySprite(&gSprites[gTasks[taskId].data[0]]);
	UnfreezeEventObjects();
	DestroyTask(taskId);
	gPlayerAvatar->preventStep = FALSE;
}

void PlayerGoThroughDoor(u8 taskId)
{
    struct Task *task = &gTasks[taskId];
    s16 *x = &task->data[2];
    s16 *y = &task->data[3];
	u8 playerObjId = gPlayerAvatar->eventObjectId;
	u8 followerObjId = GetFollowerObjectId();

    switch (task->data[0])
    {
    case 0:
		if (TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_DASH))
			SetPlayerAvatarTransitionFlags(PLAYER_AVATAR_FLAG_ON_FOOT); //Stop running

		gFollowerState.comeOutDoorStairs = FALSE; //Just in case came out and when right back in
        FreezeEventObjects();
        PlayerGetDestCoords(x, y);
        PlaySE(GetDoorSoundEffect(*x, *y - 1));
        task->data[1] = FieldAnimateDoorOpen(*x, *y - 1);
        task->data[0] = 1;
        break;
    case 1:
        if (task->data[1] < 0 || gTasks[task->data[1]].isActive != TRUE)
        {
            EventObjectClearHeldMovementIfActive(&gEventObjects[playerObjId]);
            EventObjectSetHeldMovement(&gEventObjects[playerObjId], MOVEMENT_ACTION_WALK_NORMAL_UP);
			
			if (gFollowerState.inProgress && !gEventObjects[followerObjId].invisible)
			{
				u8 newState = DetermineFollowerState(&gEventObjects[followerObjId], MOVEMENT_ACTION_WALK_NORMAL_UP,
													DetermineFollowerDirection(&gEventObjects[playerObjId], &gEventObjects[followerObjId]));
				EventObjectClearHeldMovementIfActive(&gEventObjects[followerObjId]);
				EventObjectSetHeldMovement(&gEventObjects[followerObjId], newState);
			}

            task->data[0] = 2;
        }
        break;
    case 2:
        if (walkrun_is_standing_still())
        {
			if (!gFollowerState.inProgress || gEventObjects[followerObjId].invisible) //Don't close door on follower
				task->data[1] = FieldAnimateDoorClose(*x, *y - 1);
            EventObjectClearHeldMovementIfFinished(&gEventObjects[playerObjId]);
            sub_807DCB0(0); //sub_80AF0F4
            task->data[0] = 3;
        }
        break;
    case 3:
        if (task->data[1] < 0 || gTasks[task->data[1]].isActive != TRUE)
        {
            task->data[0] = 4;
        }
        break;
    case 4:
		if (gFollowerState.inProgress)
		{
			EventObjectClearHeldMovementIfActive(&gEventObjects[followerObjId]);
			EventObjectSetHeldMovement(&gEventObjects[followerObjId], MOVEMENT_ACTION_WALK_NORMAL_UP);
		}

        TryFadeOutOldMapMusic();
        WarpFadeScreen();
        PlayRainStoppingSoundEffect();
        task->data[0] = 0;
        task->func = (void*) 0x807E719;
        break;
	case 5:
        TryFadeOutOldMapMusic();
        PlayRainStoppingSoundEffect();
        task->data[0] = 0;
        task->func = (void*) 0x807E719;
        break;
    }
}

static u8 GetPlayerFaceToDoorDirection(struct EventObject* player, struct EventObject* follower)
{
	s16 delta_x = player->currentCoords.x - follower->currentCoords.x;

	if (delta_x < 0)
		return DIR_EAST;
	else if (delta_x > 0)
		return DIR_WEST;

	return DIR_NORTH;
}

static void Task_FollowerOutOfDoor(u8 taskId)
{
	struct EventObject* follower = &gEventObjects[GetFollowerMapObjId()];
	struct EventObject* player = &gEventObjects[gPlayerAvatar->eventObjectId];
    struct Task *task = &gTasks[taskId];
    s16 x = task->data[2];
    s16 y = task->data[3];

	if (TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_DASH)
	&& EventObjectClearHeldMovementIfFinished(player))
		SetPlayerAvatarTransitionFlags(PLAYER_AVATAR_FLAG_ON_FOOT); //Temporarily stop running
		
	if (EventObjectClearHeldMovementIfFinished(player))
		EventObjectTurn(player, GetPlayerFaceToDoorDirection(player, follower)); //The player should face towards the follow as the exit the door

	switch (task->data[0]) {
		case 0:
			FreezeEventObjects();
			gTasks[taskId].data[1] = FieldAnimateDoorOpen(follower->currentCoords.x, follower->currentCoords.y);
			task->data[0] = 1;
			break;
		case 1:
			if (task->data[1] < 0 || gTasks[task->data[1]].isActive != TRUE) //Door isn't still opening
			{
				follower->invisible = FALSE;
				EventObjectTurn(follower, DIR_SOUTH); //The follower should be facing down when it comes out the door
				EventObjectSetHeldMovement(follower, MOVEMENT_ACTION_WALK_NORMAL_DOWN);
				task->data[0] = 2;
			}
			break;
		case 2:
			if (EventObjectClearHeldMovementIfFinished(follower))
			{
				task->data[1] = FieldAnimateDoorClose(x, y);
				task->data[0] = 3;
			}
			break;
		case 3:
			if (task->data[1] < 0 || gTasks[task->data[1]].isActive != TRUE) //Door is closed
			{
				UnfreezeEventObjects();
				task->data[0] = 4;
			}
			break;
		case 4:
			gFollowerState.comeOutDoorStairs = FALSE;
			gPlayerAvatar->preventStep = FALSE; //Player can move again
			DestroyTask(taskId);
        break;
	}
}

void StairsMoveFollower(void)
{
	if (!gFollowerState.inProgress)
		return;

	if (!FuncIsActiveTask(Task_FollowerHandleIndoorStairs) && gFollowerState.comeOutDoorStairs != 2)
		CreateTask(Task_FollowerHandleIndoorStairs, 1);
}

static void Task_FollowerHandleIndoorStairs(u8 taskId)
{
	struct EventObject* follower = &gEventObjects[GetFollowerMapObjId()];
	struct EventObject* player = &gEventObjects[gPlayerAvatar->eventObjectId];
    struct Task *task = &gTasks[taskId];

	switch (task->data[0]) {
		case 0:
			gFollowerState.comeOutDoorStairs = 2; //So the task doesn't get created more than once
			EventObjectClearHeldMovementIfActive(follower);
			EventObjectSetHeldMovement(follower, DetermineFollowerState(follower, MOVEMENT_ACTION_WALK_NORMAL_DOWN, DetermineFollowerDirection(player, follower)));
			task->data[0]++;
			break;
		case 1:
			if (EventObjectClearHeldMovementIfFinished(follower))
			{
				EventObjectSetHeldMovement(follower, DetermineFollowerState(follower, MOVEMENT_ACTION_WALK_SLOWEST_DOWN, player->movementDirection));
				DestroyTask(taskId);
			}
			break;
	}
}

void EscalatorMoveFollower(u8 movementType)
{
	if (!gFollowerState.inProgress)
		return;

	u8 taskId = CreateTask(Task_FollowerHandleEscalator, 1);
	gTasks[taskId].data[1] = movementType;
}

static void Task_FollowerHandleEscalator(u8 taskId)
{
	struct EventObject* follower = &gEventObjects[GetFollowerMapObjId()];
	struct EventObject* player = &gEventObjects[gPlayerAvatar->eventObjectId];
	EventObjectClearHeldMovementIfActive(follower);
	EventObjectSetHeldMovement(follower, DetermineFollowerState(follower, MOVEMENT_ACTION_WALK_SLOWEST_DOWN, DetermineFollowerDirection(player, follower)));
	DestroyTask(taskId);
}

void EscalatorMoveFollowerFinish(void)
{
	if (!gFollowerState.inProgress)
		return;

	CreateTask(Task_FollowerHandleEscalatorFinish, 1);
}

static void Task_FollowerHandleEscalatorFinish(u8 taskId)
{
	s16 x, y;
	struct EventObject* follower = &gEventObjects[GetFollowerMapObjId()];
	struct EventObject* player = &gEventObjects[gPlayerAvatar->eventObjectId];
	struct Sprite* sprite = &gSprites[follower->spriteId];
    struct Task *task = &gTasks[taskId];

	switch (task->data[0]) {
		case 0:
			MoveEventObjectToMapCoords(follower, player->currentCoords.x, player->currentCoords.y);
			PlayerGetDestCoords(&x, &y);
			task->data[2] = MapGridGetMetatileBehaviorAt(x, y);
			task->data[7] = 0;
			task->data[0]++;
			break;
		case 1:
			if (task->data[7]++ < 0x20) //Wait half a second before revealing the follower
				break;

			task->data[0]++;
			task->data[1] = 16;
			CalculateFollowerEscalatorTrajectoryUp(task);
			gFollowerState.warpEnd = 0;
			gPlayerAvatar->preventStep = TRUE;
			EventObjectSetHeldMovement(follower, GetFaceDirectionMovementAction(DIR_EAST));
			if (task->data[2] == 0x6b)
				task->data[0] = 4;
			break;
		case 2:
			follower->invisible = FALSE;
			CalculateFollowerEscalatorTrajectoryDown(task);
			task->data[0]++;
			break;
		case 3:
			CalculateFollowerEscalatorTrajectoryDown(task);
			task->data[2]++;
			if (task->data[2] & 1)
			{
				task->data[1]--;
			}

			if (task->data[1] == 0)
			{
				sprite->pos2.x = 0;
				sprite->pos2.y = 0;
				task->data[0] = 6;
			}
			break;
		case 4:
			follower->invisible = FALSE;
			CalculateFollowerEscalatorTrajectoryUp(task);
			task->data[0]++;
			break;
		case 5:
			CalculateFollowerEscalatorTrajectoryUp(task);
			task->data[2]++;
			if (task->data[2] & 1)
			{
				task->data[1]--;
			}

			if (task->data[1] == 0)
			{
				sprite->pos2.x = 0;
				sprite->pos2.y = 0;
				task->data[0]++;
			}
			break;
		case 6:
			if (EventObjectClearHeldMovementIfFinished(follower))
			{
				gPlayerAvatar->preventStep = FALSE;
				DestroyTask(taskId);
			}
	}
}

static void CalculateFollowerEscalatorTrajectoryDown(struct Task *task)
{
    struct Sprite* sprite = &gSprites[gEventObjects[GetFollowerMapObjId()].spriteId];
	sprite->pos2.x = Cosine(0x84, task->data[1]);
	sprite->pos2.y = Sine(0x94, task->data[1]);
}

static void CalculateFollowerEscalatorTrajectoryUp(struct Task *task)
{
    struct Sprite* sprite = &gSprites[gEventObjects[GetFollowerMapObjId()].spriteId];
	sprite->pos2.x = Cosine(0x7c, task->data[1]);
	sprite->pos2.y = Sine(0x76, task->data[1]);
}

void FollowMe_HandleBike(void)
{
	if (gFollowerState.currentSprite == FOLLOWER_SPRITE_INDEX_SURF) //Follower is urfing
		return; //Sprite will automatically be adjusted when they finish surfing

	if (gPlayerAvatar->flags & (PLAYER_AVATAR_FLAG_MACH_BIKE | PLAYER_AVATAR_FLAG_ACRO_BIKE)
	&& gFollowerState.flags & FOLLOWER_FLAG_CAN_BIKE)
		SetFollowerSprite(FOLLOWER_SPRITE_INDEX_BIKE); //Bike on
	else
		SetFollowerSprite(FOLLOWER_SPRITE_INDEX_NORMAL);
}

void FollowMe_HandleSprite(void)
{
	if (gPlayerAvatar->flags & (PLAYER_AVATAR_FLAG_MACH_BIKE | PLAYER_AVATAR_FLAG_ACRO_BIKE)
	&& gFollowerState.flags & FOLLOWER_FLAG_CAN_BIKE)
		SetFollowerSprite(FOLLOWER_SPRITE_INDEX_BIKE);
	else
		SetFollowerSprite(FOLLOWER_SPRITE_INDEX_NORMAL);
}

static void SetFollowerSprite(u8 spriteIndex)
{
	if (!gFollowerState.inProgress)
		return;
		
	if (gFollowerState.currentSprite == spriteIndex)
		return;

	//Save sprite
	gFollowerState.currentSprite = spriteIndex;

	struct EventObject* follower = &gEventObjects[GetFollowerMapObjId()];
	EventObjectSetGraphicsId(follower, GetFollowerSprite());

	//Reload the entire event object.
	//It would usually be enough just to change the sprite Id, but if the original
	//sprite and the new sprite have different palettes, the palette would need to
	//be reloaded.
	struct EventObject backupFollower = *follower;
	RemoveEventObject(&gEventObjects[GetFollowerMapObjId()]);

	struct EventObjectTemplate clone = *GetEventObjectTemplateByLocalIdAndMap(gFollowerState.map.id, gFollowerState.map.number, gFollowerState.map.group);
	clone.graphicsId = GetFollowerSprite();
	gFollowerState.objId = TrySpawnEventObjectTemplate(&clone, gSaveBlock1->location.mapNum, gSaveBlock1->location.mapGroup, clone.x, clone.y);

	follower = &gEventObjects[GetFollowerMapObjId()];
	*follower = backupFollower;
	MoveEventObjectToMapCoords(follower, follower->currentCoords.x, follower->currentCoords.y);
	EventObjectTurn(follower, follower->facingDirection);
}

void FollowMe_WarpSetEnd(void)
{
	if (!gFollowerState.inProgress)
		return;

	struct EventObject* player = &gEventObjects[gPlayerAvatar->eventObjectId];
	struct EventObject* follower = &gEventObjects[GetFollowerMapObjId()];

	gFollowerState.warpEnd = 1;
	PlayerLogCoordinates(player);
	
	u8 toY = gFollowerState.comeOutDoorStairs == 1 ? player->currentCoords.y - 1 : player->currentCoords.y;
	MoveEventObjectToMapCoords(follower, player->currentCoords.x, toY);

	follower->facingDirection = player->facingDirection;
	follower->movementDirection = player->movementDirection;
}

void CreateFollowerAvatar(void)
{
	if (!gFollowerState.inProgress)
		return;

	struct EventObject* player = &gEventObjects[gPlayerAvatar->eventObjectId];
	struct EventObjectTemplate clone = *GetEventObjectTemplateByLocalIdAndMap(gFollowerState.map.id, gFollowerState.map.number, gFollowerState.map.group);

	clone.graphicsId = GetFollowerSprite();
	clone.x = player->currentCoords.x - 7;
	clone.y = player->currentCoords.y - 7;

	switch (GetPlayerFacing()) {
		case DIR_NORTH:
			clone.movementType = MOVEMENT_TYPE_FACE_UP;
			break;
		case DIR_WEST:
			clone.movementType = MOVEMENT_TYPE_FACE_LEFT;
			break;
		case DIR_EAST:
			clone.movementType = MOVEMENT_TYPE_FACE_RIGHT;
			break;
	}

	// Create NPC and store ID
	gFollowerState.objId = TrySpawnEventObjectTemplate(&clone, gSaveBlock1->location.mapNum, gSaveBlock1->location.mapGroup, clone.x, clone.y);
	gFollowerState.createSurfBlob = FALSE;

	if (gFollowerState.objId == EVENT_OBJECTS_COUNT)
		gFollowerState.inProgress = FALSE; //Cancel the following because couldn't load sprite

	gEventObjects[gFollowerState.objId].invisible = TRUE;
}

static void TurnNPCIntoFollower(u8 localId, u8 followerFlags)
{
	if (gFollowerState.inProgress)
		return; //Only 1 NPC following at a time

	struct EventObject* follower;

	for (u8 eventObjId = 0; eventObjId < MAP_OBJECTS_COUNT; ++eventObjId) //For each NPC on the map
	{
		if (!gEventObjects[eventObjId].active || gEventObjects[eventObjId].isPlayer)
			continue;

		if (gEventObjects[eventObjId].localId == localId)
		{
			follower = &gEventObjects[eventObjId];
			const u8* script = GetEventObjectScriptPointerByEventObjectId(eventObjId);
			u16 flag = GetEventObjectTemplateByLocalIdAndMap(follower->localId, follower->mapNum, follower->mapGroup)->flagId;
			//gEventObjects[eventObjId].localId = gEventObjects[eventObjId].localId;

			gFollowerState.inProgress = TRUE;
			gFollowerState.objId = eventObjId;
			gFollowerState.gfxId = follower->graphicsId;
			gFollowerState.map.id = gEventObjects[eventObjId].localId;
			gFollowerState.map.number = gSaveBlock1->location.mapNum;
			gFollowerState.map.group = gSaveBlock1->location.mapGroup;
			gFollowerState.script = script;
			gFollowerState.flag = flag;
			gFollowerState.flags = followerFlags;
			gFollowerState.createSurfBlob = FALSE;
			gFollowerState.comeOutDoorStairs = FALSE;

			if (!(gFollowerState.flags & FOLLOWER_FLAG_CAN_BIKE) //Follower can't bike
			&&  TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_BIKE)) //Player on bike
				SetPlayerAvatarTransitionFlags(PLAYER_AVATAR_FLAG_ON_FOOT); //Dismmount Bike
		}
	}
}

//@Details: Sets up the follow me feature.
//@Input:	Var8000 - Local id or var containing local id of NPC to start following player.
//			Var8001 - Follower flags.
void sp0D1_SetUpFollowerSprite(void)
{
	TurnNPCIntoFollower(VarGet(Var8000), Var8001);
}

//@Details: Ends the follow me feature.
void sp0D2_DestroyFollowerSprite(void)
{
	if (gFollowerState.inProgress)
	{
		RemoveEventObject(&gEventObjects[gFollowerState.objId]);
		FlagSet(gFollowerState.flag);
		gFollowerState.inProgress = FALSE;
	}
}