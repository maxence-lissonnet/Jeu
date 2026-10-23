// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBAnimNotifyState_HitWindow.h"
#include "EMBCombatComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"

void UEMBAnimNotifyState_HitWindow::NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration, const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyBegin(MeshComp, Animation, TotalDuration, EventReference);

	// La détection est réservée au serveur : sur un client on sort avant même de chercher le composant
	// (ce notify tourne à chaque tick d'animation de chaque machine). Les clients reçoivent le résultat par réplication.
	if (!MeshComp || !MeshComp->GetOwner() || !MeshComp->GetOwner()->HasAuthority())
	{
		return;
	}
	if (UEMBCombatComponent* Combat = UEMBCombatComponent::FindFromMesh(MeshComp))
	{
		Combat->BeginHitWindow(Params);
	}
}

void UEMBAnimNotifyState_HitWindow::NotifyTick(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float FrameDeltaTime, const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyTick(MeshComp, Animation, FrameDeltaTime, EventReference);

	if (!MeshComp || !MeshComp->GetOwner() || !MeshComp->GetOwner()->HasAuthority())
	{
		return; // Serveur uniquement : aucun coût de recherche de composant sur les clients.
	}
	if (UEMBCombatComponent* Combat = UEMBCombatComponent::FindFromMesh(MeshComp))
	{
		Combat->TickHitWindow(FrameDeltaTime);
	}
}

void UEMBAnimNotifyState_HitWindow::NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyEnd(MeshComp, Animation, EventReference);

	if (!MeshComp || !MeshComp->GetOwner() || !MeshComp->GetOwner()->HasAuthority())
	{
		return;
	}
	if (UEMBCombatComponent* Combat = UEMBCombatComponent::FindFromMesh(MeshComp))
	{
		// Dernier balayage jusqu'à la pose finale, puis passage en Recovery.
		Combat->TickHitWindow(0.f);
		Combat->EndHitWindow();
	}
}

FString UEMBAnimNotifyState_HitWindow::GetNotifyName_Implementation() const
{
	return TEXT("EMB Hit Window");
}
