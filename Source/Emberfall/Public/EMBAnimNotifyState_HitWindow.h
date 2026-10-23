// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "EMBCombatTypes.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "EMBAnimNotifyState_HitWindow.generated.h"

/**
 * Fenêtre de coup : pendant ce notify (frames actives de l'attaque), le serveur balaie une sphère le long
 * de la lame (sockets de l'arme) à chaque tick d'animation. À la fin, la FSM passe en Recovery.
 * Porte les paramètres du coup : rayon de la sphère et multiplicateurs éventuels (par rapport à la définition
 * de l'attaque dans UEMBCombatComponent).
 */
UCLASS(meta = (DisplayName = "EMB Hit Window"))
class EMBERFALL_API UEMBAnimNotifyState_HitWindow : public UAnimNotifyState
{
	GENERATED_BODY()

public:
	/** Rayon (cm) de la sphère de détection, multiplicateur de dégâts et de poise de ce coup précis. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HitWindow")
	FEMBHitWindowParams Params;

	virtual void NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration, const FAnimNotifyEventReference& EventReference) override;
	virtual void NotifyTick(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float FrameDeltaTime, const FAnimNotifyEventReference& EventReference) override;
	virtual void NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;
	virtual FString GetNotifyName_Implementation() const override;
};
