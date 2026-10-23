// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBAnimNotifyState_ComboWindow.h"
#include "EMBCombatComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"

void UEMBAnimNotifyState_ComboWindow::NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration, const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyBegin(MeshComp, Animation, TotalDuration, EventReference);

	// Exécuté sur toutes les machines : le serveur consomme le buffer ; le client propriétaire s'en sert
	// pour la prédiction du coup suivant (seul le serveur a des effets de gameplay, voir SetComboWindowOpen).
	if (UEMBCombatComponent* Combat = UEMBCombatComponent::FindFromMesh(MeshComp))
	{
		Combat->SetComboWindowOpen(true);
	}
}

void UEMBAnimNotifyState_ComboWindow::NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyEnd(MeshComp, Animation, EventReference);

	if (UEMBCombatComponent* Combat = UEMBCombatComponent::FindFromMesh(MeshComp))
	{
		Combat->SetComboWindowOpen(false);
	}
}

FString UEMBAnimNotifyState_ComboWindow::GetNotifyName_Implementation() const
{
	return TEXT("EMB Combo Window");
}
