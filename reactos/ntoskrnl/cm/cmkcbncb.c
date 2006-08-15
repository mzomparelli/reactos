/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/cm/cmkcbncb.c
 * PURPOSE:         Routines for handling KCBs, NCBs, as well as key hashes.
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

ULONG CmpHashTableSize;
PCM_KEY_HASH_TABLE_ENTRY *CmpCacheTable;
PCM_NAME_HASH_TABLE_ENTRY *CmpNameCacheTable;
BOOLEAN CmpAllocInited;
KGUARDED_MUTEX CmpAllocBucketLock, CmpDelayAllocBucketLock;
LIST_ENTRY CmpFreeKCBListHead;
ULONG CmpDelayedCloseSize;
ULONG CmpDelayedCloseElements;
KGUARDED_MUTEX CmpDelayedCloseTableLock;
BOOLEAN CmpDelayCloseWorkItemActive;
LIST_ENTRY CmpDelayedLRUListHead;
LIST_ENTRY CmpFreeDelayItemsListHead;
ULONG CmpDelayCloseIntervalInSeconds = 5;
KDPC CmpDelayCloseDpc;
KTIMER CmpDelayCloseTimer;
BOOLEAN CmpHoldLazyFlush;

/* FUNCTIONS *****************************************************************/

VOID
NTAPI
CmpRemoveKeyHash(IN PCM_KEY_HASH KeyHash)
{
    PCM_KEY_HASH *Prev;
    PCM_KEY_HASH Current;

    /* Lookup all the keys in this index entry */
    Prev = &GET_HASH_ENTRY(CmpCacheTable, KeyHash->ConvKey).Entry;
    while (TRUE)
    {
        /* Save the current one and make sure it's valid */
        Current = *Prev;
        ASSERT(Current != NULL);

        /* Check if it matches */
        if (Current == KeyHash)
        {
            /* Then write the previous one */
            *Prev = Current->NextHash;
            break;
        }

        /* Otherwise, keep going */
        Prev = &Current->NextHash;
    }
}

PCM_KEY_CONTROL_BLOCK
NTAPI
CmpInsertKeyHash(IN PCM_KEY_HASH KeyHash,
                 IN BOOLEAN IsFake)
{
    ULONG i;
    PCM_KEY_HASH Entry;

    /* Get the hash index */
    i = GET_HASH_INDEX(KeyHash->ConvKey);

    /* If this is a fake key, increase the key cell to use the parent data */
    if (IsFake) KeyHash->KeyCell++;

    /* Loop the hash table */
    Entry = CmpCacheTable[i]->Entry;
    while (Entry)
    {
        /* Check if this matches */
        if ((KeyHash->ConvKey == Entry->ConvKey) &&
            (KeyHash->KeyCell == Entry->KeyCell) &&
            (KeyHash->KeyHive == Entry->KeyHive))
        {
            /* Return it */
            return CONTAINING_RECORD(Entry, CM_KEY_CONTROL_BLOCK, KeyHash);
        }

        /* Keep looping */
        Entry = Entry->NextHash;
    }

    /* No entry found, add this one and return NULL since none existed */
    KeyHash->NextHash = CmpCacheTable[i]->Entry;
    CmpCacheTable[i]->Entry = KeyHash;
    return NULL;
}

// FIXME: NEEDS COMPLETION
PCM_NAME_CONTROL_BLOCK
NTAPI
CmpGetNcb(IN PUNICODE_STRING NodeName)
{
    PCM_NAME_CONTROL_BLOCK Ncb;
    ULONG ConvKey = 0;
    PWCHAR p, pp;
    ULONG i;
    BOOLEAN IsCompressed = TRUE, Found;
    PCM_NAME_HASH HashEntry;
    ULONG Length;

    /* Loop the name */
    p = NodeName->Buffer;
    for (i = 0; i < NodeName->Length; i += sizeof(WCHAR))
    {
        /* Make sure it's not a slash */
        if (*p != OBJ_NAME_PATH_SEPARATOR)
        {
            /* Add it to the hash */
            ConvKey = 37 * ConvKey + RtlUpcaseUnicodeChar(*p);
        }

        /* Next character */
        p++;
    }

    /* Set assumed lengh and loop to check */
    Length = NodeName->Length / sizeof(WCHAR);
    for (i = 0; i < (NodeName->Length / sizeof(WCHAR)); i++)
    {
        /* Check if this is a valid character */
        if (*NodeName->Buffer > (UCHAR)-1)
        {
            /* This is the actual size, and we know we're not compressed */
            Length = NodeName->Length;
            IsCompressed = FALSE;
        }
    }

    /* Get the hash entry */
    HashEntry = GET_HASH_ENTRY(CmpNameCacheTable, ConvKey).Entry;
    while (HashEntry)
    {
        /* Get the current NCB */
        Ncb = CONTAINING_RECORD(HashEntry, CM_NAME_CONTROL_BLOCK, NameHash);

        /* Check if the hash matches */
        if ((ConvKey = HashEntry->ConvKey) && (Length = Ncb->NameLength))
        {
            /* Assume success */
            Found = TRUE;

            /* If the NCB is compressed, do a compressed name compare */
            if (Ncb->Compressed)
            {
                /* Compare names */
                if (CmpCompareCompressedName(NodeName, Ncb->Name, Length))
                {
                    /* We failed */
                    Found = FALSE;
                }
            }
            else
            {
                /* Do a manual compare */
                p = NodeName->Buffer;
                pp = Ncb->Name;
                for (i = 0; i < Ncb->NameLength; i += sizeof(WCHAR))
                {
                    /* Compare the character */
                    if (RtlUpcaseUnicodeChar(*p) != RtlUpcaseUnicodeChar(*pp))
                    {
                        /* Failed */
                        Found = FALSE;
                        break;
                    }

                    /* Next chars */
                    *p++;
                    *pp++;
                }
            }

            /* Check if we found a name */
            if (Found)
            {

            }
        }

        /* Go to the next hash */
        HashEntry = HashEntry->NextHash;
    }
}

BOOLEAN
NTAPI
CmpTryToConvertKcbSharedToExclusive(IN PCM_KEY_CONTROL_BLOCK Kcb)
{
    /* Convert the lock */
    ASSERT(CmpIsKcbLockedExclusive(Kcb) == FALSE);
    if (ExConvertPushLockSharedToExclusive(GET_HASH_ENTRY(CmpCacheTable,
                                                          Kcb->ConvKey).Lock))
    {
        /* Set the lock owner */
        GET_HASH_ENTRY(CmpCacheTable, Kcb->ConvKey).Owner = KeGetCurrentThread();
        return TRUE;
    }

    /* We failed */
    return FALSE;
}

VOID
NTAPI
CmpRemoveKcb(IN PCM_KEY_CONTROL_BLOCK Kcb)
{
    /* Make sure that the registry and KCB are utterly locked */
    ASSERT((CmpIsKcbLockedExclusive(Kcb) == TRUE) ||
           (CmpTestRegistryLockExclusive() == TRUE));

    /* Remove the key hash */
    CmpRemoveKeyHash(&Kcb->KeyHash);
}

VOID
NTAPI
CmpFreeKcb(IN PCM_KEY_CONTROL_BLOCK Kcb)
{
    ULONG i;
    PCM_ALLOC_PAGE AllocPage;
    PAGED_CODE();

    /* Sanity checks */
    ASSERT(IsListEmpty(&(Kcb->KeyBodyListHead)) == TRUE);
    for (i = 0; i < 4; i++) ASSERT(Kcb->KeyBodyArray[i] == NULL);

    /* Check if it wasn't privately allocated */
    if (!Kcb->PrivateAlloc)
    {
        /* Free it from the pool */
        ExFreePool(Kcb);
    }
    else
    {
        /* Acquire the private allocation lock */
        KeAcquireGuardedMutex(&CmpAllocBucketLock);

        /* Sanity check on lock ownership */
        ASSERT((GET_HASH_ENTRY(CmpCacheTable, Kcb->ConvKey).Owner ==
                KeGetCurrentThread()) ||
               (CmpTestRegistryLockExclusive() == TRUE));

        /* Remove us from the list */
        RemoveEntryList(&CmpFreeKCBListHead);

        /* Get the allocation page */
        AllocPage = (PCM_ALLOC_PAGE)((ULONG_PTR)Kcb & 0xFFFFF000);

        /* Sanity check */
        ASSERT(AllocPage->FreeCount != CM_KCBS_PER_PAGE);

        /* Increase free count */
        if (++AllocPage->FreeCount == CM_KCBS_PER_PAGE)
        {
            /* Loop all the entries */
            for (i = CM_KCBS_PER_PAGE; i; i--)
            {
                /* Remove the entry 
                RemoveEntryList(& */
            }

            /* Free the page */
            ExFreePool(AllocPage);
        }

        /* Release the lock */
        KeReleaseGuardedMutex(&CmpAllocBucketLock);
    }
}

BOOLEAN
NTAPI
CmpReferenceKcb(IN PCM_KEY_CONTROL_BLOCK Kcb)
{
    /* Check if this is the KCB's first reference */
    if (Kcb->RefCount == 0)
    {
        /* Check if the KCB is locked in shared mode */
        if (!CmpIsKcbLockedExclusive(Kcb))
        {
            /* Convert it to exclusive */
            if (!CmpTryToConvertKcbSharedToExclusive(Kcb))
            {
                /* Set the delete flag */
                Kcb->Delete = TRUE;

                /* Increase the reference count while we release the lock */
                InterlockedIncrement((PLONG)&Kcb->RefCount);

                /* Sanity check, KCB should still be shared */
                ASSERT(CmpIsKcbLockedExclusive(Kcb) == FALSE);

                /* Release the current lock */
                CmpReleaseKcbLock(Kcb);

                /* Now acquire the lock in exclusive mode */
                CmpAcquireKcbLockExclusive(Kcb);

                /* Decrement the reference count; the lock is now held again */
                InterlockedDecrement((PLONG)&Kcb->RefCount);

                /* Sanity check */
                ASSERT((Kcb->DelayedCloseIndex == CmpDelayedCloseSize) ||
                       (Kcb->DelayedCloseIndex == 0));

                /* Remove the delete flag */
                Kcb->Delete = FALSE;
            }
        }
    }

    /* Increase the reference count */
    if ((InterlockedIncrement((PLONG)&Kcb->RefCount) + 1) == 0)
    {
        /* We've overflown to 64K references, bail out */
        InterlockedDecrement((PLONG)&Kcb->RefCount);
        return FALSE;
    }

    /* Check if this was the last close index */
    if (!Kcb->DelayedCloseIndex)
    {
        /* Check if the KCB is locked in shared mode */
        if (!CmpIsKcbLockedExclusive(Kcb))
        {
            /* Convert it to exclusive */
            if (!CmpTryToConvertKcbSharedToExclusive(Kcb))
            {
                /* Sanity check, KCB should still be shared */
                ASSERT(CmpIsKcbLockedExclusive(Kcb) == FALSE);

                /* Release the current lock */
                CmpReleaseKcbLock(Kcb);

                /* Now acquire the lock in exclusive mode */
                CmpAcquireKcbLockExclusive(Kcb);
            }
        }

        /* If we're still the last entry, remove us */
        if (!Kcb->DelayedCloseIndex) CmpRemoveFromDelayedClose(Kcb);
    }

    /* Return success */
    return TRUE;
}

// FIXME: THIS FUNCTION IS PARTIALLY FUCKED
PCM_DELAYED_CLOSE_ENTRY
NTAPI
CmpAllocateDelayItem(VOID)
{
    PCM_DELAYED_CLOSE_ENTRY Entry;
    PCM_ALLOC_PAGE AllocPage;
    ULONG i;
    PLIST_ENTRY NextEntry;
    PAGED_CODE();

    /* Lock the allocation buckets */
    KeAcquireGuardedMutex(&CmpDelayAllocBucketLock);
    if (TRUE)
    {
        /* Allocate an allocation page */
        AllocPage = ExAllocatePoolWithTag(PagedPool, PAGE_SIZE, TAG_CM);
        if (AllocPage)
        {
            /* Set default entries */
            AllocPage->FreeCount = CM_DELAYS_PER_PAGE;

            /* Loop each entry */
            for (i = 0; i < CM_DELAYS_PER_PAGE; i++)
            {
                /* Get this entry and link it */
                Entry = (PCM_DELAYED_CLOSE_ENTRY)(&AllocPage[i]);
                InsertHeadList(&Entry->DelayedLRUList,
                               &CmpFreeDelayItemsListHead);
            }
        }

        /* Get the entry and the alloc page */
        Entry = CONTAINING_RECORD(NextEntry,
                                  CM_DELAYED_CLOSE_ENTRY,
                                  DelayedLRUList);
        AllocPage = (PCM_ALLOC_PAGE)((ULONG_PTR)Entry & 0xFFFFF000);

        /* Decrease free entries */
        ASSERT(AllocPage->FreeCount != 0);
        AllocPage->FreeCount--;

        /* Release the lock */
        KeReleaseGuardedMutex(&CmpDelayAllocBucketLock);
        return Entry;
    }

    /* Release the lock */
    KeReleaseGuardedMutex(&CmpDelayAllocBucketLock);
    return Entry;
}

VOID
NTAPI
CmpArmDelayedCloseTimer(VOID)
{
    LARGE_INTEGER Timeout;
    PAGED_CODE();

    /* Setup the interval */
    Timeout.QuadPart = CmpDelayCloseIntervalInSeconds * -10000000;
    KeSetTimer(&CmpDelayCloseTimer, Timeout, &CmpDelayCloseDpc);
}

VOID
NTAPI
CmpAddToDelayedClose(IN PCM_KEY_CONTROL_BLOCK Kcb,
                     IN BOOLEAN LockHeldExclusively)
{
    ULONG i;
    ULONG OldRefCount, NewRefCount;
    PCM_DELAYED_CLOSE_ENTRY Entry;
    PAGED_CODE();

    /* Sanity check */
    ASSERT((CmpIsKcbLockedExclusive(Kcb) == TRUE) ||
           (CmpTestRegistryLockExclusive() == TRUE));

    /* Make sure it's valid */
    if (Kcb->DelayedCloseIndex != CmpDelayedCloseSize) ASSERT(FALSE);

    /* Sanity checks */
    ASSERT(Kcb->RefCount == 0);
    ASSERT(IsListEmpty(&Kcb->KeyBodyListHead) == TRUE);
    for (i = 0; i < 4; i++) ASSERT(Kcb->KeyBodyArray[i] == NULL);

    /* Allocate a delay item */
    Entry = CmpAllocateDelayItem();
    if (!Entry)
    {
        /* Cleanup immediately */
        CmpCleanUpKcbCacheWithLock(Kcb, LockHeldExclusively);
        return;
    }

    /* Sanity check */
    if (Kcb->InDelayClose) ASSERT(FALSE);

    /* Get the previous reference count */
    OldRefCount = Kcb->InDelayClose;
    ASSERT(OldRefCount == 0);

    /* Write the new one */
    NewRefCount = InterlockedCompareExchange(&Kcb->InDelayClose,
                                             1,
                                             OldRefCount);
    if (NewRefCount != OldRefCount) ASSERT(FALSE);

    /* Remove the delete flag */
    Kcb->Delete = FALSE;

    /* Set up the close entry */
    Kcb->DelayCloseEntry = Entry;
    Entry->KeyControlBlock = Kcb;

    /* Increase the number of elements */
    InterlockedIncrement(&CmpDelayedCloseElements);

    /* Acquire the delayed close table lock */
    KeAcquireGuardedMutex(&CmpDelayedCloseTableLock);

    /* Insert the entry into the list */
    InsertHeadList(&CmpDelayedLRUListHead, &Entry->DelayedLRUList);

    /* Check if we need to enable anything */
    if ((CmpDelayedCloseElements > CmpDelayedCloseSize) &&
        !(CmpDelayCloseWorkItemActive))
    {
        /* Yes, we have too many elements to close, and no work item */
        CmpArmDelayedCloseTimer();
    }

    /* Release the table lock */
    KeReleaseGuardedMutex(&CmpDelayedCloseTableLock);
}

PCM_KEY_CONTROL_BLOCK
NTAPI
CmpAllocateKcb(VOID)
{
    PLIST_ENTRY ListHead, NextEntry;
    PCM_KEY_CONTROL_BLOCK CurrentKcb;
    PCM_ALLOC_PAGE AllocPage;
    ULONG i;
    PAGED_CODE();

    /* Check if private allocations are initialized */
    if (CmpAllocInited)
    {
        /* They are, acquire the bucket lock */
        KeAcquireGuardedMutex(&CmpAllocBucketLock);

        /* Loop the free KCB list */
        ListHead = &CmpFreeKCBListHead;
        NextEntry = ListHead->Flink;
        while (NextEntry != ListHead)
        {
            /* Remove the entry */
            RemoveEntryList(NextEntry);

            /* Get the KCB */
            CurrentKcb = CONTAINING_RECORD(NextEntry,
                                           CM_KEY_CONTROL_BLOCK,
                                           FreeListEntry);

            /* Get the allocation page */
            AllocPage = (PCM_ALLOC_PAGE)((ULONG_PTR)CurrentKcb & 0xFFFFF000);

            /* Decrease the free count */
            ASSERT(AllocPage->FreeCount != 0);
            AllocPage->FreeCount--;

            /* Make sure this KCB is privately allocated */
            ASSERT(CurrentKcb->PrivateAlloc == 1);

            /* Release the allocation lock */
            KeReleaseGuardedMutex(&CmpAllocBucketLock);

            /* Return the KCB */
            return CurrentKcb;
        }

        /* Allocate an allocation page */
        AllocPage = ExAllocatePoolWithTag(PagedPool, PAGE_SIZE, TAG_CM);
        if (AllocPage)
        {
            /* Set default entries */
            AllocPage->FreeCount = CM_KCBS_PER_PAGE;

            /* Loop each entry */
            for (i = 0; i < CM_KCBS_PER_PAGE; i++)
            {
                /* Get this entry */
                CurrentKcb = (PCM_KEY_CONTROL_BLOCK)(AllocPage + 1);

                /* Set it up */
                CurrentKcb->PrivateAlloc = TRUE;
                CurrentKcb->DelayCloseEntry = NULL;
                InsertHeadList(&CurrentKcb->FreeListEntry,
                               &CmpFreeKCBListHead);
            }

            /* Get the KCB */
            CurrentKcb = CONTAINING_RECORD(NextEntry,
                                           CM_KEY_CONTROL_BLOCK,
                                           FreeListEntry);

            /* Get the allocation page */
            AllocPage = (PCM_ALLOC_PAGE)((ULONG_PTR)CurrentKcb & 0xFFFFF000);

            /* Decrease the free count */
            ASSERT(AllocPage->FreeCount != 0);
            AllocPage->FreeCount--;

            /* Make sure this KCB is privately allocated */
            ASSERT(CurrentKcb->PrivateAlloc == 1);

            /* Release the allocation lock */
            KeReleaseGuardedMutex(&CmpAllocBucketLock);

            /* Return the KCB */
            return CurrentKcb;
        }

        /* Release the lock */
        KeReleaseGuardedMutex(&CmpAllocBucketLock);
    }

    /* Allocate a KCB only */
    CurrentKcb = ExAllocatePoolWithTag(PagedPool,
                                       sizeof(CM_KEY_CONTROL_BLOCK),
                                       TAG_CM);
    if (CurrentKcb)
    {
        /* Set it up */
        CurrentKcb->PrivateAlloc = 0;
        CurrentKcb->DelayCloseEntry = NULL;
    }

    /* Return it */
    return CurrentKcb;
}

VOID
NTAPI
CmpDereferenceKcbWithLock(IN PCM_KEY_CONTROL_BLOCK Kcb,
                          IN BOOLEAN LockHeldExclusively)
{
    /* Sanity check */
    ASSERT((CmpIsKcbLockedExclusive(Kcb) == TRUE) ||
           (CmpTestRegistryLockExclusive() == TRUE));

    /* Check if we should do a direct delete */
    if (((CmpHoldLazyFlush) &&
         !(Kcb->ExtFlags & CM_KCB_SYM_LINK_FOUND) &&
         !(Kcb->Flags & KEY_SYM_LINK)) ||
        (Kcb->ExtFlags & CM_KCB_NO_DELAY_CLOSE) ||
        (Kcb->PrivateAlloc))
    {
        /* Clean up the KCB*/
        CmpCleanUpKcbCacheWithLock(Kcb, LockHeldExclusively);
    }
    else
    {
        /* Otherwise, use delayed close */
        CmpAddToDelayedClose(Kcb, LockHeldExclusively);
    }
}

VOID
NTAPI
CmpInitializeKcbKeyBodyList(IN PCM_KEY_CONTROL_BLOCK Kcb)
{
    /* Initialize the list */
    InitializeListHead(&Kcb->KeyBodyListHead);

    /* Clear the bodies */
    Kcb->KeyBodyArray[0] =
    Kcb->KeyBodyArray[1] =
    Kcb->KeyBodyArray[2] =
    Kcb->KeyBodyArray[3] = NULL;
}

PCM_KEY_CONTROL_BLOCK
NTAPI
CmpCreateKcb(IN PHHIVE Hive,
             IN HCELL_INDEX Index,
             IN PCM_KEY_NODE Node,
             IN PCM_KEY_CONTROL_BLOCK Parent,
             IN ULONG Flags,
             IN PUNICODE_STRING KeyName)
{
    PCM_KEY_CONTROL_BLOCK Kcb, FoundKcb = NULL;
    UNICODE_STRING NodeName;
    ULONG ConvKey = 0, i;
    BOOLEAN IsFake, HashLock;
    PWCHAR p;

    /* Make sure we own this hive */
    if (((PCMHIVE)Hive)->CreatorOwner != KeGetCurrentThread()) return NULL;

    /* Check if this is a fake KCB */
    IsFake = (BOOLEAN)(Flags & CMP_CREATE_FAKE_KCB);

    /* If we have a parent, use its ConvKey */
    if (Parent) ConvKey = Parent->ConvKey;

    /* Make a copy of the name */
    NodeName = *KeyName;

    /* Remove leading slash */
    while ((NodeName.Length) && (*NodeName.Buffer == OBJ_NAME_PATH_SEPARATOR))
    {
        /* Move the buffer by one */
        NodeName.Buffer++;
        NodeName.Length -= sizeof(WCHAR);
    }

    /* Make sure we didn't get just a slash or something */
    ASSERT(NodeName.Length > 0);

    /* Now setup the hash */
    p = NodeName.Buffer;
    for (i = 0; i < NodeName.Length; i += sizeof(WCHAR))
    {
        /* Make sure it's a valid character */
        if ((*p != OBJ_NAME_PATH_SEPARATOR) && (*p))
        {
            /* Add this key to the hash */
            ConvKey = 37 * ConvKey + RtlUpcaseUnicodeChar(*p);
        }

        /* Move on */
        p++;
    }

    /* Allocate the KCB */
    Kcb = CmpAllocateKcb();
    if (!Kcb) return NULL;

    /* Initailize the key list */
    CmpInitializeKcbKeyBodyList(Kcb);

    /* Set it up */
    Kcb->Delete = FALSE;
    Kcb->RefCount = 1;
    Kcb->KeyHive = Hive;
    Kcb->KeyCell = Index;
    Kcb->ConvKey = ConvKey;
    Kcb->DelayedCloseIndex = CmpDelayedCloseSize;

    /* Check if we have two hash entires */
    HashLock = (BOOLEAN)(Flags & CMP_LOCK_HASHES_FOR_KCB);
    if (HashLock)
    {
        /* Not yet implemented */
        KeBugCheck(0);
    }

    /* Check if we already have a KCB */
    FoundKcb = CmpInsertKeyHash(&Kcb->KeyHash, IsFake);
    if (FoundKcb)
    {
        /* Sanity check */
        ASSERT(!FoundKcb->Delete);

        /* Free the one we allocated and reference this one */
        CmpFreeKcb(Kcb);
        Kcb = FoundKcb;
        if (!CmpReferenceKcb(Kcb))
        {
            /* We got too many handles */
            ASSERT(Kcb->RefCount + 1 != 0);
            Kcb = NULL;
        }
        else
        {
            /* Check if we're not creating a fake one, but it used to be fake */
            if ((Kcb->ExtFlags & CM_KCB_KEY_NON_EXIST) && !(IsFake))
            {
                /* Set the hive and cell */
                Kcb->KeyHive = Hive;
                Kcb->KeyCell = Index;

                /* This means that our current information is invalid */
                Kcb->ExtFlags = CM_KCB_INVALID_CACHED_INFO;
            }

            /* Check if we didn't have any valid data */
            if (!(Kcb->ExtFlags & (CM_KCB_NO_SUBKEY |
                                   CM_KCB_SUBKEY_ONE |
                                   CM_KCB_SUBKEY_HINT)))
            {
                /* Calculate the index hint */
                Kcb->SubKeyCount = Node->SubKeyCounts[0] +
                                   Node->SubKeyCounts[1];

                /* Cached information is now valid */
                Kcb->ExtFlags &= ~CM_KCB_INVALID_CACHED_INFO;
            }

            /* Setup the other data */
            Kcb->KcbLastWriteTime = Node->LastWriteTime;
            Kcb->KcbMaxNameLen = (USHORT)Node->MaxNameLen;
            Kcb->KcbMaxValueNameLen = (USHORT)Node->MaxValueNameLen;
            Kcb->KcbMaxValueDataLen = Node->MaxValueDataLen;
        }
    }
    else
    {
        /* No KCB, do we have a parent? */
        if (Parent)
        {
            /* Reference the parent */
            if (((Parent->TotalLevels + 1) < 512) && (CmpReferenceKcb(Parent)))
            {
                /* Link it */
                Kcb->ParentKcb = Parent;
                Kcb->TotalLevels = Parent->TotalLevels + 1;
            }
            else
            {
                /* Remove the KCB and free it */
                CmpRemoveKcb(Kcb);
                CmpFreeKcb(Kcb);
                Kcb = NULL;
            }
        }
        else
        {
            /* No parent, this is the root node */
            Kcb->ParentKcb = NULL;
            Kcb->TotalLevels = 1;
        }

        /* Check if we have a KCB */
        if (Kcb)
        {
            /* Get the NCB */
            Kcb->NameBlock = CmpGetNcb(&NodeName);
            if (Kcb->NameBlock)
            {
                /* Fill it out */
                Kcb->ValueCache.Count = Node->ValueList.Count;
                Kcb->ValueCache.ValueList = Node->ValueList.List;
                Kcb->Flags = Node->Flags;
                Kcb->ExtFlags = 0;
                Kcb->DelayedCloseIndex = CmpDelayedCloseSize;

                /* Remember if this is a fake key */
                if (IsFake) Kcb->ExtFlags |= CM_KCB_KEY_NON_EXIST;

                /* Setup the other data */
                Kcb->SubKeyCount = Node->SubKeyCounts[0] +
                                   Node->SubKeyCounts[1];
                Kcb->KcbLastWriteTime = Node->LastWriteTime;
                Kcb->KcbMaxNameLen = (USHORT)Node->MaxNameLen;
                Kcb->KcbMaxValueNameLen = (USHORT)Node->MaxValueNameLen;
                Kcb->KcbMaxValueDataLen = (USHORT)Node->MaxValueDataLen;
            }
            else
            {
                /* Dereference the KCB */
                CmpDereferenceKcbWithLock(Parent, FALSE);

                /* Remove the KCB and free it */
                CmpRemoveKcb(Kcb);
                CmpFreeKcb(Kcb);
                Kcb = NULL;
            }
        }
    }

    /* Check if we had locked the hashes */
    if (HashLock)
    {
        /* Not yet implemented: Unlock hashes */
        KeBugCheck(0);
    }

    /* Return the KCB */
    return Kcb;
}

