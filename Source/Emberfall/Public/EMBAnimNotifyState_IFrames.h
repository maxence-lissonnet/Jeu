// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "EMBAnimNotifyState_IFrames.generated.h"

/**
 * I-frames : tant que ce notify est actif, le personnage porte le tag d'état State.Combat.Invulnerable
 * (aucun coup ne le touche). Placé dans les montages de roulade : frames 3-15 (léger), 3-13 (moyen),
 * 3-11 (lourd). Le champ StateTag permet aussi de s'en servir pour State.Combat.HyperArmor.
 */
UCLASS(meta = (DisplayName = "EMB I-Frames"))
class EMBERFALL_API UEMBAnimNotifyState_IFrames : public UAnimNotifyState
{
	GENERATED_BODY()

public:
	/** Tag appliqué pendant la fenêtre. Vide = State.Combat.Invulnerable. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "IFrames")
	FGameplayTag StateTag;

	virtual void NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration, const FAnimNotifyEventReference& EventReference) override;
	virtual void NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;
	virtual FString GetNotifyName_Implementation() const override;
};
