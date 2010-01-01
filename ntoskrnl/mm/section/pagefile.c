/*
 * Copyright (C) 1998-2005 ReactOS Team (and the authors from the programmers section)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 *
 * PROJECT:         ReactOS kernel
 * FILE:            ntoskrnl/mm/section.c
 * PURPOSE:         Implements section objects
 *
 * PROGRAMMERS:     Rex Jolliff
 *                  David Welch
 *                  Eric Kohl
 *                  Emanuele Aliberti
 *                  Eugene Ingerman
 *                  Casper Hornstrup
 *                  KJK::Hyperion
 *                  Guido de Jong
 *                  Ge van Geldorp
 *                  Royce Mitchell III
 *                  Filip Navara
 *                  Aleksey Bragin
 *                  Jason Filby
 *                  Thomas Weidenmueller
 *                  Gunnar Andre' Dalsnes
 *                  Mike Nordell
 *                  Alex Ionescu
 *                  Gregor Anich
 *                  Steven Edwards
 *                  Herve Poussineau
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>
#include <reactos/exeformat.h>

#if defined (ALLOC_PRAGMA)
#pragma alloc_text(INIT, MmCreatePhysicalMemorySection)
#pragma alloc_text(INIT, MmInitSectionImplementation)
#endif

extern KEVENT MmWaitPageEvent;

NTSTATUS
NTAPI
MmCreatePageFileSection
(PROS_SECTION_OBJECT *SectionObject,
 ACCESS_MASK DesiredAccess,
 POBJECT_ATTRIBUTES ObjectAttributes,
 PLARGE_INTEGER UMaximumSize,
 ULONG SectionPageProtection,
 ULONG AllocationAttributes)
/*
 * Create a section which is backed by the pagefile
 */
{
   LARGE_INTEGER MaximumSize;
   PROS_SECTION_OBJECT Section;
   PMM_SECTION_SEGMENT Segment;
   NTSTATUS Status;

   if (UMaximumSize == NULL)
   {
      return(STATUS_UNSUCCESSFUL);
   }
   MaximumSize = *UMaximumSize;

   /*
    * Create the section
    */
   Status = ObCreateObject(ExGetPreviousMode(),
                           MmSectionObjectType,
                           ObjectAttributes,
                           ExGetPreviousMode(),
                           NULL,
                           sizeof(ROS_SECTION_OBJECT),
                           0,
                           0,
                           (PVOID*)(PVOID)&Section);
   if (!NT_SUCCESS(Status))
   {
      return(Status);
   }

   /*
    * Initialize it
    */
   Section->SectionPageProtection = SectionPageProtection;
   Section->AllocationAttributes = AllocationAttributes;
   Section->Segment = NULL;
   Section->FileObject = NULL;
   Section->MaximumSize = MaximumSize;
   Segment = ExAllocatePoolWithTag(NonPagedPool, sizeof(MM_SECTION_SEGMENT),
                                   TAG_MM_SECTION_SEGMENT);
   if (Segment == NULL)
   {
	  ObDereferenceObject(Section);
      return(STATUS_NO_MEMORY);
   }
   Section->Segment = Segment;
   Segment->FileObject = NULL;
   Segment->ReferenceCount = 1;
   ExInitializeFastMutex(&Segment->Lock);
   Segment->Protection = SectionPageProtection;
   Segment->RawLength.QuadPart = MaximumSize.QuadPart;
   Segment->Length.QuadPart = PAGE_ROUND_UP(MaximumSize.QuadPart);
   Segment->Flags = MM_PAGEFILE_SEGMENT;
   Segment->WriteCopy = FALSE;
   MiInitializeSectionPageTable(Segment);
   *SectionObject = Section;
   return(STATUS_SUCCESS);
}

PFN_TYPE
NTAPI
MmWithdrawSectionPage
(PMM_SECTION_SEGMENT Segment, PLARGE_INTEGER FileOffset, BOOLEAN *Dirty)
{
	ULONG Entry = MiGetPageEntrySectionSegment(Segment, FileOffset);

	*Dirty = !!IS_DIRTY_SSE(Entry);

	DPRINT("Withdraw %x (%x) of %wZ\n", FileOffset->LowPart, Entry, Segment->FileObject ? &Segment->FileObject->FileName : NULL);

	if (MM_IS_WAIT_PTE(Entry)) return MM_WAIT_ENTRY;
	if (Entry && !IS_SWAP_FROM_SSE(Entry))
	{
		MiSetPageEntrySectionSegment(Segment, FileOffset, MAKE_SWAP_SSE(MM_WAIT_ENTRY));
		return PFN_FROM_SSE(Entry);
	}
	else return 0;
}

NTSTATUS
NTAPI
MmFinalizeSectionPageOut
(PMM_SECTION_SEGMENT Segment, PLARGE_INTEGER FileOffset, PFN_TYPE Page,
 BOOLEAN Dirty)
{
	NTSTATUS Status = STATUS_SUCCESS;
	SWAPENTRY Swap;

	if (Dirty)
	{
		DPRINT("Finalize (dirty) Segment %x\n", Segment);
		DPRINT("Segment->FileObject %x\n", Segment->FileObject);
		DPRINT("Segment->Flags %x\n", Segment->Flags);
		if (Segment->FileObject && !(Segment->Flags & MM_IMAGE_SEGMENT))
		{
			DPRINT("Segment %x FileObject %x\n", Segment, Segment->FileObject, &Segment);
			Status = MiWriteBackPage(Segment->FileObject, FileOffset, PAGE_SIZE, Page);
			if (NT_SUCCESS(Status))
			{
				DPRINT("Write zero to %x:%x %x\n", Segment, FileOffset->LowPart, Page);
				MmLockSectionSegment(Segment);
				MiSetPageEntrySectionSegment(Segment, FileOffset, 0);
				MmUnlockSectionSegment(Segment);
			}
		}
		else
		{
			DPRINT("Getting swap page\n");
			Swap = MmAllocSwapPage();
			if (!Swap) 
				Status = STATUS_PAGEFILE_QUOTA;
			else
			{
				DPRINT("Writing to swap page\n");
				Status = MmWriteToSwapPage(Swap, Page);
			}

			DPRINT("Status %x\n", Status);
			if (NT_SUCCESS(Status)) 
			{
				DPRINT("Set Swap entry\n");
				MmLockSectionSegment(Segment);
				MiSetPageEntrySectionSegment(Segment, FileOffset, MAKE_SWAP_SSE(Swap));
				MmUnlockSectionSegment(Segment);
			}
		}
	}
	else
	{
		DPRINT("Write zero to %x:%x (%x)\n", Segment, FileOffset->LowPart, Page);
		MmLockSectionSegment(Segment);
		MiSetPageEntrySectionSegment(Segment, FileOffset, 0);
		MmUnlockSectionSegment(Segment);
	}

	DPRINT("Status %x\n", Status);
	if (NT_SUCCESS(Status))
	{
		MmSetSavedSwapEntryPage(Page, 0);
		MmDereferencePage(Page);
	}

	KeSetEvent(&MmWaitPageEvent, IO_NO_INCREMENT, FALSE);

	DPRINT("Status %x\n", Status);
	return Status;
}
