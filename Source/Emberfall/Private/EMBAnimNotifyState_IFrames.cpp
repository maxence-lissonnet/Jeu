// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBAnimNotifyState_IFrames.h"
#include "EMBCombatComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"

void UEMBAnimNotifyState_IFrames::NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration, const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyBegin(MeshComp, Animation, TotalDuration, EventReference);

	// Le compteur est tenu sur toutes les machines, mais seul le serveur s'en sert pour résoudre les coups.
	if (UEMBCombatComponent* Combat = UEMBCombatComponent::FindFromMesh(MeshComp))
	{
		Combat->BeginStateTag(StateTag);
	}
}

void UEMBAnimNotifyState_IFrames::NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyEnd(MeshComp, Animation, EventReference);

	if (UEMBCombatComponent* Combat = UEMBCombatComponent::FindFromMesh(MeshComp))
	{
		Combat->EndStateTag(StateTag);
	}
}

FString UEMBAnimNotifyState_IFrames::GetNotifyName_Implementation() const
{
	return TEXT("EMB I-Frames");
}
